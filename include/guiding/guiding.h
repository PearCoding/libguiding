#ifndef LIBGUIDING_GUIDING_H
#define LIBGUIDING_GUIDING_H

#include <atomic>
#include <fstream>
#include <iostream>
#include <array>
#include <cassert>
#include <mutex>
#include <random>

#ifdef __CUDACC__
#define GUIDING_CPU_GPU __host__ __device__
#else
/**
 * @brief Allows functions and methods to be executed both on the CPU and the GPU.
 * @note Only defined when __CUDACC__ is set.
 */
#define GUIDING_CPU_GPU
#endif

namespace guiding {

template<typename T>
void writeType(std::ostream &os, const T &) {
    auto name = typeid(T).name();
    uint16_t len = strlen(name);

    os.write((const char *)&len, sizeof(len));
    os.write(name, len);
}

template<typename T>
void readType(std::istream &is, T &) {
    uint16_t len;
    is.read((char *)&len, sizeof(len));

    char name[len+1];
    is.read((char *)name, len);
    name[len] = 0;

    const char *expected = typeid(T).name();
    if (strcmp(name, expected)) {
        std::cerr << "expected to read " << expected << ", but found " << name << std::endl;
        assert(false);
    }
}

template<typename T>
struct has_custom_io {
    template<typename U, void (U::*)(std::ostream &) const> struct SFINAE {};
    template<typename U> static char Test(SFINAE<U, &U::write>*);
    template<typename U> static int Test(...);
    static const bool value = sizeof(Test<T>(0)) == sizeof(char);
};

/**
 * Writes an element to disk.
 * Override this if you store complex objects in your distribution that
 * need special serialization procedures.
 */
template<typename T>
void write(std::ostream &os, const T &t) {
    if constexpr(has_custom_io<T>::value) {
        //writeType(os, t);
        t.write(os);
    } else {
        os.write((const char *)&t, sizeof(T));
    }
}

/**
 * Reads an element to disk.
 * Override this if you store complex objects in your distribution that
 * need special deserialization procedures.
 */
template<typename T>
void read(std::istream &is, T &t) {
    if constexpr(has_custom_io<T>::value) {
        //readType(is, t);
        t.read(is);
    } else {
        is.read((char *)&t, sizeof(T));
    }
}

template<typename V>
class atomic {
public:
    atomic() {}
    atomic(const V &v) : m_value(v) {}
    atomic(const atomic<V> &other) {
        m_value = other.m_value;
    }

    void operator+=(const V &v) {
        std::unique_lock lock(m_mutex);
        m_value += v;
    }

    void operator=(const V &value) {
        m_value = value;
    }

    void operator=(const atomic<V> &other) {
        m_value = other.m_value;
    }

    void operator+=(const atomic<V> &other) {
        m_value += other.m_value;
    }

    operator V() const { return m_value; }

    V operator/(Float other) { return m_value / other; }
    V operator*(Float other) { return m_value * other; }

    const V &value() const { return m_value; }

    void write(std::ostream &os) const {
        guiding::write(os, m_value);
    }

    void read(std::istream &is) {
        guiding::read(is, m_value);
    }

private:
    V m_value;
    std::mutex m_mutex;
};

#ifdef __CUDACC__
template<>
class atomic<Float> {
public:
    atomic() {}

    atomic(const atomic<Float> &other) {
        *this = other;
    }

    operator Float() const { return m_value; }

    GUIDING_CPU_GPU void operator=(const Float &value) {
        m_value = value;
    }

    GUIDING_CPU_GPU void operator=(const atomic<Float> &other) {
        m_value = other;
    }

    GUIDING_CPU_GPU void operator+=(const Float &value) {
        atomicAdd(&m_value, value);
    }

    void write(std::ostream &os) const {
        guiding::write(os, m_value);
    }

    void read(std::istream &is) {
        guiding::read(is, m_value);
    }

protected:
    Float m_value;
};
#else
template<>
class atomic<Float> : public std::atomic<Float> {
public:
    atomic() : std::atomic<Float>() {}

    atomic(const atomic<Float> &other) : std::atomic<Float>() {
        *this = other;
    }

    GUIDING_CPU_GPU void operator=(const Float &value) {
        store(value, std::memory_order_relaxed);
    }

    GUIDING_CPU_GPU void operator=(const atomic<Float> &other) {
        *this = other.load();
    }

    GUIDING_CPU_GPU void operator+=(const Float &value) {
        auto current = load();
        while (!compare_exchange_weak(current, current + value));
    }

    void write(std::ostream &os) const {
        Float value = load();
        guiding::write(os, value);
    }

    void read(std::istream &is) {
        Float value;
        guiding::read(is, value);
        store(value);
    }
};
#endif

template<int D>
GUIDING_CPU_GPU Float computeOverlap(const VectorXf<D> &min1, const VectorXf<D> &max1, const VectorXf<D> &min2, const VectorXf<D> &max2) {
    // @todo this ignores the fact that a hypervolume can extend beyond the [0,1) interval
    // using this directly will give you a bias if you are not using leaf reweighting
    // (directions at the corners will have smaller weights)

    Float overlap = 1;
    for (int i = 0; i < D; ++i)
        overlap *= std::max(std::min(max1[i], max2[i]) - std::max(min1[i], min2[i]), Float(0));
    return overlap;
}

template<typename ...Args>
struct is_empty {
    enum { value = 0 };
};

template<>
struct is_empty<> {
    enum { value = 1 };
};

//
// these meta-programming hacks might look horrendous,
// but they are not quite as horrible as OpenGL!
//

template<typename...>
struct RecurseChild {
    typedef void Type;
};

template<typename Child, typename Head, typename ...Tail>
struct RecurseChild<Child, Head, Tail...> {
    typedef typename RecurseChild<Child, Tail...>::Type::Child Type;
};

template<typename Child>
struct RecurseChild<Child> {
    typedef Child Type;
};

static inline Float random() {
    // @todo some people might want to override this!
    static std::default_random_engine generator;
    return std::generate_canonical<Float, std::numeric_limits<Float>::digits>(generator);
}

}

#endif
