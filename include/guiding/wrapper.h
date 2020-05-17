#ifndef HUSSAR_GUIDING_POINTWRAPPER_H
#define HUSSAR_GUIDING_POINTWRAPPER_H

#include <guiding/guiding.h>

#include <array>
#include <vector>
#include <fstream>
#include <cstring>
#include <cassert>

#include <mutex>
#include <shared_mutex>

namespace guiding {

// Wrapper<Sample>

template<typename S, typename C>
class Wrapper {
public:
    typedef S Sample;
    typedef C Distribution;
    typedef typename Distribution::Vector Vector;
    typedef typename Distribution::Aux Aux;

    struct Settings {
        Float uniformProb = 0.5f;
        typename Distribution::Settings child;
    };

    Settings settings;

    Wrapper() {
        reset();
    }

    Wrapper(const Settings &settings)
    : settings(settings) {
        reset();
    }

    void operator=(const Wrapper<S, C> &other) {
        settings   = other.settings;
        m_sampling = other.m_sampling;
        m_training = other.m_training;
        
        m_samplesSoFar  = other.m_samplesSoFar.load();
        m_nextMilestone = other.m_nextMilestone;
    }

    void reset() {
        std::unique_lock lock(m_mutex);

        m_training = Distribution();
        m_sampling = Distribution();

        m_samplesSoFar  = 0;
        m_nextMilestone = 1024;
    }

    template<typename ...Args>
    Float sample(Vector &x, Args&&... params) {
        if (settings.uniformProb == 1)
            return 1.f;
        
        std::shared_lock lock(m_mutex);

        Float pdf = 1 - settings.uniformProb;
        if (x[0] < settings.uniformProb) {
            x[0] /= settings.uniformProb;
            pdf *= m_sampling.pdf(
                settings.child,
                x,
                std::forward<Args>(params)...
            );
        } else {
            x[0] -= settings.uniformProb;
            x[0] /= 1 - settings.uniformProb;
            m_sampling.sample(
                settings.child,
                x,
                std::forward<Args>(params)...,
                pdf
            );
        }

        pdf += settings.uniformProb;
        return pdf;
    }

    template<typename ...Args>
    Float pdf(Args&&... params) const {
        if (settings.uniformProb == 1)
            return 1.f;
        
        std::shared_lock lock(m_mutex);
        return settings.uniformProb + (1 - settings.uniformProb) * m_sampling.pdf(
            settings.child,
            std::forward<Args>(params)...
        );

        return 1;
    }

    template<typename ...Args>
    void splat(const Sample &sample, Float weight, Args&&... params) {
        //if (settings.uniformProb == 1)
        //    return;
        
        {
            Aux aux;
            Float density = guiding::target(sample, aux);

            std::shared_lock lock(m_mutex);
            m_training.splat(
                settings.child,
                density, aux, weight,
                std::forward<Args>(params)...
            );
        }

        if (++m_samplesSoFar > m_nextMilestone) {
            // it's wednesday my dudes!
            step();
        }
    }

    size_t samplesSoFar() const { return m_samplesSoFar; }

    Distribution &training() { return m_training; }
    const Distribution &training() const { return m_training; }

    Distribution &sampling() { return m_sampling; }
    const Distribution &sampling() const { return m_sampling; }

private:
    void step() {
        std::unique_lock lock(m_mutex);
        if (m_samplesSoFar < m_nextMilestone)
            // someone was here before us!
            return;
        
        m_training.build(settings.child);
        m_sampling = m_training;
        m_training.refine(settings.child);

        m_nextMilestone *= 2;
    }

    Distribution m_sampling, m_training;

    std::atomic<size_t> m_samplesSoFar;
    size_t m_nextMilestone;

    mutable std::shared_mutex m_mutex;
};

}

#endif