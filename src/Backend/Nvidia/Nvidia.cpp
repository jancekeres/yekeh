// Copyright (c) 2019, Zpalmtree
//
// Please see the included LICENSE file for more information.

//////////////////////////////////
#include "Backend/Nvidia/Nvidia.h"
//////////////////////////////////

#include <iostream>

#include "ArgonVariants/Variants.h"
#include "Backend/Nvidia/NvidiaHash.h"
#include "Utilities/ColouredMsg.h"
#include "Nvidia/Argon2.h"

Nvidia::Nvidia(
    const HardwareConfig &hardwareConfig,
    const std::function<void(const JobSubmit &jobSubmit)> &submitValidHashCallback,
    const std::function<void(
        const uint32_t hashesPerformed,
        const std::string &deviceName)> &incrementHashesPerformedCallback):
    m_hardwareConfig(hardwareConfig),
    m_submitValidHash(submitValidHashCallback),
    m_incrementHashesPerformed(incrementHashesPerformedCallback)
{
    std::copy_if(
        hardwareConfig.nvidia.devices.begin(),
        hardwareConfig.nvidia.devices.end(),
        std::back_inserter(m_availableDevices),
        [](const NvidiaDevice &device)
        {
            return device.enabled;
        }
    );

    m_numAvailableGPUs = m_availableDevices.size();
}

void Nvidia::start(const Job &job, const uint32_t initialNonce)
{
    if (!m_threads.empty())
    {
        stop();
    }

    m_shouldStop = false;

    m_nonce = initialNonce;

    m_currentJob = job;

    /* Indicate that there's no new jobs available to other threads */
    m_newJobAvailable = std::vector<bool>(m_numAvailableGPUs, false);

    for (uint32_t i = 0; i < m_numAvailableGPUs; i++)
    {
        m_threads.push_back(std::thread(&Nvidia::hash, this, m_availableDevices[i], i));
    }
}

void Nvidia::stop()
{
    m_shouldStop = true;

    for (int i = 0; i < m_numAvailableGPUs; i++)
    {
        m_newJobAvailable[i] = true;
    }

    /* Wait for all the threads to stop */
    for (auto &thread : m_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    /* Empty the threads vector for later re-creation */
    m_threads.clear();
}

void Nvidia::setNewJob(const Job &job, const uint32_t initialNonce)
{
    /* Set new nonce */
    m_nonce = initialNonce;

    /* Update stored job */
    m_currentJob = job;

    /* Indicate to each thread that there's a new job */
    for (int i = 0; i < m_numAvailableGPUs; i++)
    {
        m_newJobAvailable[i] = true;
    }
}

std::vector<PerformanceStats> Nvidia::getPerformanceStats()
{
    return {};
}

std::shared_ptr<NvidiaHash> getNvidiaMiningAlgorithm(const std::string &algorithm)
{
    switch(ArgonVariant::algorithmNameToCanonical(algorithm))
    {
        case ArgonVariant::Chukwa:
        {
            return std::make_shared<NvidiaHash>(512, 3);
        }
        case ArgonVariant::ChukwaWrkz:
        {
            return std::make_shared<NvidiaHash>(256, 4);
        }
        default:
        {
            throw std::runtime_error("Developer fucked up. Sorry!");
        }
    }
}

void Nvidia::hash(const NvidiaDevice gpu, const uint32_t threadNumber)
{
    NvidiaState state;

    std::string currentAlgorithm;

    const std::string gpuName = gpu.name + "-" + std::to_string(gpu.id);

    while (!m_shouldStop)
    {
        Job job = m_currentJob;

        auto algorithm = getNvidiaMiningAlgorithm(job.algorithm);

        /* New job, reinitialize memory, etc */
        if (job.algorithm != currentAlgorithm)
        {
            freeState(state);

            m_hardwareConfig.initNonceOffsets(algorithm->getMemory());

            state = initializeState(
                gpu.id,
                algorithm->getMemory(),
                algorithm->getIterations()
            );

            currentAlgorithm = job.algorithm;
        }

        state.isNiceHash = job.isNiceHash;

        std::vector<uint8_t> salt(job.rawBlob.begin(), job.rawBlob.begin() + 16);

        uint32_t startNonce = m_nonce + m_hardwareConfig.nvidia.devices[gpu.id].nonceOffset;

        if (job.isNiceHash)
        {
            startNonce = (startNonce & 0x00FFFFFF) | (*job.nonce() & 0xFF000000);
        }

        initJob(state, job.rawBlob, salt, startNonce, job.target);

        /* Let the algorithm perform any necessary initialization */
        algorithm->init(state);

        while (!m_newJobAvailable[threadNumber])
        {
            try
            {
                const auto hashResult = algorithm->hash(startNonce);

                /* Increment the number of hashes we performed so the hashrate
                   printer is accurate */
                m_incrementHashesPerformed(state.launchParams.noncesPerRun, gpuName);

                /* Woot, found a valid share, submit it */
                if (hashResult.success)
                {
                    m_submitValidHash({ hashResult.hash, job.jobID, hashResult.nonce, job.target, gpuName });
                }

                /* Increment nonce for next block */
                startNonce += m_hardwareConfig.noncesPerRound;
            }
            catch (const std::exception &e)
            {
                std::cout << WarningMsg("Caught unexpected error from GPU hasher: " + std::string(e.what())) << std::endl;

                try
                {
                    freeState(state);
                }
                /* Free state will probably fail if we got a cuda exception */
                catch (const std::exception &)
                {
                }

                return;
            }
        }

        /* Switch to new job. */
        m_newJobAvailable[threadNumber] = false;
    }

    freeState(state);
}