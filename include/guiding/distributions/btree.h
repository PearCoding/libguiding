#ifndef HUSSAR_GUIDING_BTREEDISTRIBUTION_H
#define HUSSAR_GUIDING_BTREEDISTRIBUTION_H

#include <guiding/guiding.h>

#include <array>
#include <vector>
#include <fstream>
#include <cstring>
#include <cassert>

namespace guiding {

template<int D, typename V>
class BTreeDistribution {
public:
    static constexpr auto Dimension = D;
    static constexpr auto Arity = 1 << D;

    typedef V Value;
    typedef VectorXf<Dimension> Vector;

protected:
    struct TreeNode {
        /**
         * Indexed by a bitstring, where each bit describes the slab for one of the
         * vector dimensions. Bit 0 means lower half [0, 0.5) and bit 1 means upper half
         * [0.5, 1.0).
         * The MSB corresponds to the last dimension of the vector.
         */
        std::array<int, Arity> children;
        Float density;

        atomic<Value> value; // the accumulation of the estimator (i.e., sum of integrand*weight)
        atomic<Float> weight; // the acculumation of the sample weights (i.e., sum of weight)

        bool isLeaf() const {
            return children[0] == 0;
        }

        void markAsLeaf() {
            children[0] = 0;
        }

        void splat(const Value &value, Float weight, bool secondMoment) {
            assert(weight >= 0);
            assert(value >= 0);

            this->weight += weight;
            this->value  += value * weight;

            Float target = guiding::target(value);
            if (secondMoment)
                target *= target;
            this->density += target * weight;
        }

        void reset() {
            density = 0;
            value   = Value();
            weight  = 0;
        }

        int depth(const std::vector<TreeNode> &nodes) const {
            if (isLeaf())
                return 1;

            int maxDepth = 0;
            for (int i = 0; i < Arity; ++i)
                maxDepth = std::max(maxDepth, nodes[children[i]].depth(nodes));
            return maxDepth + 1;
        }

        void write(std::ostream &os) const {
            write(os, density);
            write(os, value);
            write(os, weight);
            write(os, children);
        }

        void read(std::istream &is) {
            read(is, density);
            read(is, value);
            read(is, weight);
            read(is, children);
        }
    };

    std::vector<TreeNode> m_nodes;

    Float m_splitThreshold = 0.002f;
    bool m_leafReweighting = true;
    bool m_doFiltering = true; // box filter [Müller et al.]
    bool m_secondMoment = false; // second moment [Rath et al.]

public:
    BTreeDistribution() {
        // haven't learned anything yet, resort to uniform sampling
        setUniform();
    }

    std::string typeId() const {
        return std::string("BTreeDistribution<") + std::to_string(Dimension) + ", " + typeid(Value).name() + ">";
    }

    // accessors for building settings

    Float &splitThreshold() { return m_splitThreshold; }
    const Float &splitThreshold() const { return m_splitThreshold; }

    bool &leafReweighting() { return m_leafReweighting; }
    const bool &leafReweighting() const { return m_leafReweighting; }

    bool &doFiltering() { return m_doFiltering; }
    const bool &doFiltering() const { return m_doFiltering; }

    bool &secondMoment() { return m_secondMoment; }
    const bool &secondMoment() const { return m_secondMoment; }

    // methods for reading from the tree

    const atomic<Value> &at(const Vector &x) const {
        return m_nodes[indexAt(x)].value;
    }

    Float pdf(const Vector &x) const {
        return m_nodes[indexAt(x)].density;
    }

    const atomic<Value> &sample(Vector &x, Float &pdf) const {
        pdf = 1;

        Vector base;
        memset(base.data(), 0, sizeof(Vector));

        Float scale = 1;
        int index = 0;
        while (!m_nodes[index].isLeaf()) {
            int childIndex = 0;

            // sample each axis individually to determine sampled child
            for (int dim = 0; dim < Dimension; ++dim) {
                // marginalize over remaining dimensions {dim+1..Dimension-1}
                Float p[2] = { 0, 0 };
                for (int child = 0; child < (1 << (Dimension - dim)); ++child) {
                    // we are considering only children that match all our
                    // chosen dimensions {0..dim-1} so far.
                    // we are collecting the sum of density for children with
                    // x[dim] = 0 in p[0], and x[dim] = 1 in p[1].
                    int ci = (child << dim) | childIndex;
                    p[child & 1] += m_nodes[m_nodes[index].children[ci]].density;
                }
                
                p[0] /= p[0] + p[1];
                assert(p[0] >= 0 && p[1] >= 0);

                int slab = x[dim] > p[0];
                childIndex |= slab << dim;

                if (slab) {
                    base[dim] += 0.5 * scale;
                    x[dim] = (x[dim] - p[0]) / (1 - p[0]);
                } else
                    x[dim] = x[dim] / p[0];
            }

            auto newIndex = m_nodes[index].children[childIndex];
            assert(newIndex > index);
            index = newIndex;

            scale /= 2;
        }

        pdf *= m_nodes[index].density;
        assert(m_nodes[index].density > 0);
        
        for (int dim = 0; dim < Dimension; ++dim) {
            x[dim] *= scale;
            x[dim] += base[dim];
        }
        
        return m_nodes[index].value;
    }

    // methods for writing to the tree

    void splat(const Vector &x, const Value &value, Float weight) {
        if (!m_doFiltering) {
            m_nodes[indexAt(x)].splat(value, weight, m_secondMoment);
            return;
        }

        int depth;
        indexAt(x, depth);
        Float size = 1 / Float(1 << depth);
        
        Vector originMin, originMax, zero;
        for (int dim = 0; dim < Dimension; ++dim) {
            originMin[dim] = x[dim] - size/2;
            originMax[dim] = x[dim] + size/2;
            zero[dim] = 0;
        }
        
        splatFiltered(
            0,
            originMin, originMax,
            zero, 1.f,
            value, weight / (size * size)
        );
    }

    /**
     * Rebuilds the entire tree, making sure that leaf nodes that received
     * too few samples are pruned.
     * After building, each leaf node will have a value that is an estimate over
     * the mean value over the leaf node size (i.e., its size has been cancelled out).
     */
    void build() {
        std::vector<TreeNode> newNodes;
        newNodes.reserve(m_nodes.size());
        
        build(0, newNodes);
        if (newNodes[0].weight <= 0 || newNodes[0].density == 0) {
            // you're building a tree without samples. good luck with that.
            setUniform();
            return;
        }
        
        // normalize density
        m_nodes = newNodes;
        Float norm = m_nodes[0].density;

        for (auto &node : m_nodes) {
            node.density /= norm;
            if (!m_leafReweighting)
                node.value = node.value / m_nodes[0].weight;
        }
    }

    void refine(size_t index = 0, Float scale = 1) {
        if (m_nodes[index].isLeaf()) {
            Float criterion = m_nodes[index].density / scale;
            if (criterion >= splitThreshold())
                split(index);
            else {
                m_nodes[index].reset();
                return;
            }
        }
        
        for (int child = 0; child < Arity; ++child)
            refine(m_nodes[index].children[child], scale * Arity);
    }

    // methods that provide statistics

    int depth() const {
        return m_nodes[0].depth(m_nodes);
    }

    size_t nodeCount() const {
        return m_nodes.size();
    }

    const atomic<Value> &estimate() const {
        return m_nodes[0].value;
    }

private:
    void setUniform() {
        m_nodes.resize(1);
        m_nodes[0].markAsLeaf();
        m_nodes[0].density = 1;
        m_nodes[0].value   = Value();
        m_nodes[0].weight  = 0;
    }

    size_t indexAt(const Vector &y) const {
        int depth;
        return indexAt(y, depth);
    }

    size_t indexAt(const Vector &y, int &depth) const {
        Vector x = y;

        int index = 0;
        depth = 0;
        while (!m_nodes[index].isLeaf()) {
            int childIndex = 0;

            for (int dim = 0; dim < Dimension; ++dim) {
                int slab = x[dim] >= 0.5;
                childIndex |= slab << dim;

                if (slab)
                    x[dim] -= 0.5;
                x[dim] *= 2;
            }

            auto newIndex = m_nodes[index].children[childIndex];
            assert(newIndex > index);
            index = newIndex;

            ++depth;
        }

        return index;
    }

    void splatFiltered(
        int index,
        const Vector &originMin, const Vector &originMax,
        const Vector &nodeMin, Float nodeSize,
        const Value &value, Float weight
    ) {
        Vector nodeMax = nodeMin;
        for (int dim = 0; dim < Dimension; ++dim)
            nodeMax[dim] += nodeSize;

        Float overlap = computeOverlap<Dimension>(originMin, originMax, nodeMin, nodeMax);
        if (overlap > 0) {
            auto &node = m_nodes[index];
            if (node.isLeaf()) {
                node.splat(value, weight * overlap, m_secondMoment);
                return;
            }

            for (int child = 0; child < Arity; ++child) {
                Vector childMin = nodeMin;
                Float childSize = nodeSize / 2;

                for (int dim = 0; dim < Dimension; ++dim)
                    if (child & (1 << dim))
                        childMin[dim] += childSize;
                
                splatFiltered(
                    node.children[child],
                    originMin, originMax,
                    childMin, childSize,
                    value,
                    weight
                );
            }
        }
    }

    /**
     * Executes the first pass of building the m_nodes.
     * Parts of the tree that received no samples will be pruned (if requested via m_leafReweighting).
     * Each node in the tree will receive a value that is the mean over its childrens' values.
     * After this pass, the density of each node will correspond to the average weight within it,
     * i.e., after this pass you must still normalize the densities.
     */
    void build(size_t index, std::vector<TreeNode> &newNodes, Float scale = 1) {
        auto &node = m_nodes[index];

        // insert ourself into the tree
        size_t newIndex = newNodes.size();
        newNodes.push_back(node);

        if (node.isLeaf()) {
            auto &newNode = newNodes[newIndex];

            if (m_leafReweighting && node.weight < 1e-3) { // @todo why 1e-3?
                // node received too few samples
                newNode.weight = -1;
                return;
            }

            Float w = m_leafReweighting ? 1 / node.weight : scale;
            assert(w >= 0);

            newNode.markAsLeaf();
            newNode.density = node.density * w;
            newNode.value   = node.value   * w;
            newNode.weight  = node.weight;

            if (m_secondMoment)
                newNode.density = std::sqrt(newNode.density);
            
            return;
        }

        int validCount = 0;
        Float density  = 0;
        Float weight   = 0;
        Value value    = Value();

        for (int child = 0; child < Arity; ++child) {
            auto newChildIndex = newNodes.size();
            build(node.children[child], newNodes, scale * Arity);
            newNodes[newIndex].children[child] = newChildIndex;

            auto &newChild = newNodes[newChildIndex];
            if (newChild.weight >= 0) {
                density += newChild.density;
                value   += newChild.value;
                weight  += newChild.weight;

                ++validCount;
            }
        }

        if (!m_leafReweighting)
            // ignore that children are broken if we are using naive building
            validCount = 4;
        
        if (validCount == 0) {
            // none of the children were valid (received samples)
            // mark this node and its subtree as invalid
            newNodes[newIndex].weight = -1;
            return;
        }
        
        // density and value are both normalized according to node area
        newNodes[newIndex].density = density / validCount;
        newNodes[newIndex].value   = value   / validCount;
        newNodes[newIndex].weight  = weight;

        if (validCount < Arity) {
            // at least one of the node's children is invalid (has not received enough samples)
            newNodes.resize(newIndex + 1); // remove the subtree of this node...
            newNodes[newIndex].markAsLeaf(); // ...and replace it by a leaf node
        }
    }

    void split(size_t parentIndex) {
        size_t childIndex = m_nodes.size();
        assert(childIndex > parentIndex);
        assert(m_nodes[parentIndex].isLeaf());

        for (int child = 0; child < Arity; ++child)
            // insert new children
            m_nodes.push_back(m_nodes[parentIndex]);

        for (int child = 0; child < Arity; ++child)
            // register new children
            m_nodes[parentIndex].children[child] = childIndex + child;
    }

public:
    void write(std::ostream &os) const {
        size_t childCount = m_nodes.size();
        write(os, childCount);
        for (auto &node : m_nodes)
            node.write(os);
    }

    void read(std::istream &is) {
        size_t childCount = m_nodes.size();
        read(is, childCount);
        m_nodes.resize(childCount);

        for (auto &node : m_nodes)
            node.read(is);
    }
};

}

#endif
