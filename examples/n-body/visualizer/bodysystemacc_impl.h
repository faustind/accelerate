/*
 * Copyright 1993-2012 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

#include <helper_cuda.h>
#include <cuda.h>
#include <assert.h>
#include <math.h>
#include <memory.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <GL/glew.h>

#include <cuda_gl_interop.h>

#include "HsFFI.h"

#include "Visualize_stub.h"

BodySystemAcc::BodySystemAcc(unsigned int numBodies, unsigned int p, unsigned int q, bool usePBO, bool useSysMem)
    : m_numBodies(numBodies),
      m_bInitialized(false),
      m_bUsePBO(usePBO),
      m_bUseSysMem(useSysMem),
      m_currentRead(0),
      m_currentWrite(1),
      m_p(p),
      m_q(q)
{
    m_hPos[0] = m_hPos[1] = 0;
    m_hVel = 0;

    _initialize(numBodies);
    setSoftening(0.00125f);
    setDamping(0.995f);
}

BodySystemAcc::~BodySystemAcc()
{
    _finalize();
    m_numBodies = 0;
}

void BodySystemAcc::_initialize(int numBodies)
{
    assert(!m_bInitialized);

    m_numBodies = numBodies;

    unsigned int memSize = sizeof(float) * 4 * numBodies;

    m_deviceData.numBodies = numBodies;

    if (m_bUseSysMem)
    {
        checkCudaErrors(cudaHostAlloc((void **)&m_hPos[0], memSize, cudaHostAllocMapped | cudaHostAllocPortable));
        checkCudaErrors(cudaHostAlloc((void **)&m_hPos[1], memSize, cudaHostAllocMapped | cudaHostAllocPortable));
        checkCudaErrors(cudaHostAlloc((void **)&m_hVel,    memSize, cudaHostAllocMapped | cudaHostAllocPortable));

        memset(m_hPos[0], 0, memSize);
        memset(m_hPos[1], 0, memSize);
        memset(m_hVel, 0, memSize);

        checkCudaErrors(cudaEventCreate(&m_deviceData.event));
        checkCudaErrors(cudaHostGetDevicePointer((void **)&m_deviceData.dPos[0], (void *)m_hPos[0], 0));
        checkCudaErrors(cudaHostGetDevicePointer((void **)&m_deviceData.dPos[1], (void *)m_hPos[1], 0));
        checkCudaErrors(cudaHostGetDevicePointer((void **)&m_deviceData.dVel, (void *)m_hVel, 0));
    }
    else
    {
        m_hPos[0] = new float[m_numBodies*4];
        m_hVel = new float[m_numBodies*4];

        memset(m_hPos[0], 0, memSize);
        memset(m_hVel, 0, memSize);

        checkCudaErrors(cudaEventCreate(&m_deviceData.event));

        if (m_bUsePBO)
        {
            // create the position pixel buffer objects for rendering
            // we will actually compute directly from this memory in CUDA too
            glGenBuffers(2, (GLuint *)m_pbo);

            for (int i = 0; i < 2; ++i)
            {
                glBindBuffer(GL_ARRAY_BUFFER, m_pbo[i]);
                glBufferData(GL_ARRAY_BUFFER, memSize, m_hPos[0], GL_DYNAMIC_DRAW);

                int size = 0;
                glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, (GLint *)&size);

                if ((unsigned)size != memSize)
                {
                    fprintf(stderr, "WARNING: Pixel Buffer Object allocation failed!n");
                }

                glBindBuffer(GL_ARRAY_BUFFER, 0);
                checkCudaErrors(cudaGraphicsGLRegisterBuffer(&m_pGRes[i],
                                                             m_pbo[i],
                                                             cudaGraphicsMapFlagsNone));
            }
        }
        else
        {
            checkCudaErrors(cudaMalloc((void **)&m_deviceData.dPos[0], memSize));
            checkCudaErrors(cudaMalloc((void **)&m_deviceData.dPos[1], memSize));
        }

        checkCudaErrors(cudaMalloc((void **)&m_deviceData.dVel, memSize));
    }

    int argc = 0;
    char *argv[] = {  NULL };
    char **pargv = argv;

    // Initialize Haskell runtime
    hs_init(&argc, &pargv);

    m_ctx = NULL;
    cuCtxGetCurrent(&m_ctx);

    if (m_ctx == NULL) {
        fprintf(stderr, "Unable to get current CUDA context\n");
        exit(1);
    }

    int devID;
    checkCudaErrors(cudaGetDevice(&devID));

    m_hndl = accelerateCreate(devID, m_ctx);

    m_program = stepBodies_compile(m_hndl);

    //Allocate space for timestep and softening factor
    checkCudaErrors(cudaMalloc((void **)&m_timestep, sizeof(float)));
    checkCudaErrors(cudaMalloc((void **)&m_softening, sizeof(float)));
    m_bInitialized = true;
}

void BodySystemAcc::_finalize()
{
    assert(m_bInitialized);

    if (m_bUseSysMem)
    {
        checkCudaErrors(cudaFreeHost(m_hPos[0]));
        checkCudaErrors(cudaFreeHost(m_hPos[1]));
        checkCudaErrors(cudaFreeHost(m_hVel));

        cudaEventDestroy(m_deviceData.event);
    }
    else
    {
        delete [] m_hPos[0];
        delete [] m_hPos[1];
        delete [] m_hVel;

        checkCudaErrors(cudaFree((void **)m_deviceData.dVel));

        if (m_bUsePBO)
        {
            checkCudaErrors(cudaGraphicsUnregisterResource(m_pGRes[0]));
            checkCudaErrors(cudaGraphicsUnregisterResource(m_pGRes[1]));
            glDeleteBuffers(2, (const GLuint *)m_pbo);
        }
        else
        {
            checkCudaErrors(cudaFree((void **)m_deviceData.dPos[0]));
            checkCudaErrors(cudaFree((void **)m_deviceData.dPos[1]));
        }
    }

    freeProgram(m_program);
    accelerateDestroy(m_hndl);

    hs_exit();

    m_bInitialized = false;
}

void BodySystemAcc::loadTipsyFile(const std::string &filename)
{
    if (m_bInitialized)
        _finalize();

    std::vector< vec4<float>::Type > positions;
    std::vector< vec4<float>::Type > velocities;
    std::vector< int > ids;

    int nBodies = 0;
    int nFirst=0, nSecond=0, nThird=0;

    read_tipsy_file(positions,
                    velocities,
                    ids,
                    filename,
                    nBodies,
                    nFirst,
                    nSecond,
                    nThird);

    _initialize(nBodies);

    setArray(BODYSYSTEM_POSITION, (float *)&positions[0]);
    setArray(BODYSYSTEM_VELOCITY, (float *)&velocities[0]);
}


void BodySystemAcc::setSoftening(float softening)
{
    checkCudaErrors(cudaMemcpy(m_softening, &softening, sizeof(float), cudaMemcpyHostToDevice));
}

void BodySystemAcc::setDamping(float damping)
{
    m_damping = damping;
}

void BodySystemAcc::update(float deltaTime)
{
    assert(m_bInitialized);

    if (m_bUsePBO)
    {
        checkCudaErrors(cudaGraphicsResourceSetMapFlags(m_pGRes[m_currentRead], cudaGraphicsMapFlagsReadOnly));
        checkCudaErrors(cudaGraphicsResourceSetMapFlags(m_pGRes[1-m_currentRead], cudaGraphicsMapFlagsWriteDiscard));
        checkCudaErrors(cudaGraphicsMapResources(2, m_pGRes, 0));
        size_t bytes;
        checkCudaErrors(cudaGraphicsResourceGetMappedPointer((void **)&(m_deviceData.dPos[m_currentRead]), &bytes, m_pGRes[m_currentRead]));
        checkCudaErrors(cudaGraphicsResourceGetMappedPointer((void **)&(m_deviceData.dPos[1-m_currentRead]), &bytes, m_pGRes[1-m_currentRead]));
    }

    //Copy timestep to device.
    checkCudaErrors(cudaMemcpy(m_timestep, &deltaTime, sizeof(float), cudaMemcpyHostToDevice));

    //Marshal the input
    float* t[] = { m_timestep };
    float* s[] = { m_softening };
    int sht[] = { 0 }; //The timestep and softening factor is contained within a rank zero array
    float* p[] = { m_deviceData.dPos[m_currentRead] };
    float* v[] = { m_deviceData.dVel };
    int shb[] = { m_numBodies * 4};

    InputArray in[] = { {sht, (void**) t}, {sht, (void**) s}, {shb, (void**) p}, {shb, (void**) v} };

    ResultArray out[2];

    //Run the computation
    runProgram(m_hndl, m_program, in, out);

    float* ptrP;
    float* ptrV;

    getDevicePtrs(m_hndl, out[0], &ptrP);
    getDevicePtrs(m_hndl, out[1], &ptrV);

    checkCudaErrors(cudaMemcpy(m_deviceData.dPos[m_currentWrite], ptrP, 4 * m_numBodies * sizeof(float), cudaMemcpyDeviceToDevice));
    checkCudaErrors(cudaMemcpy(m_deviceData.dVel, ptrV, 4 * m_numBodies * sizeof(float), cudaMemcpyDeviceToDevice));

    //free old data
    freeResult(out[0]);
    freeResult(out[1]);

    if (m_bUsePBO)
    {
        checkCudaErrors(cudaGraphicsUnmapResources(2, m_pGRes, 0));
    }

    std::swap(m_currentRead, m_currentWrite);
}

float *BodySystemAcc::getArray(BodyArray array)
{
    assert(m_bInitialized);

    float *hdata = 0;
    float *ddata = 0;

    cudaGraphicsResource *pgres = NULL;

    int currentReadHost = m_bUseSysMem ? m_currentRead : 0;

    switch (array)
    {
        default:
        case BODYSYSTEM_POSITION:
            hdata = m_hPos[currentReadHost];
            ddata = m_deviceData.dPos[m_currentRead];

            if (m_bUsePBO)
            {
                pgres = m_pGRes[m_currentRead];
            }

            break;

        case BODYSYSTEM_VELOCITY:
            hdata = m_hVel;
            ddata = m_deviceData.dVel;
            break;
    }

    if (!m_bUseSysMem)
    {
        if (pgres)
        {
            checkCudaErrors(cudaGraphicsResourceSetMapFlags(pgres, cudaGraphicsMapFlagsReadOnly));
            checkCudaErrors(cudaGraphicsMapResources(1, &pgres, 0));
            size_t bytes;
            checkCudaErrors(cudaGraphicsResourceGetMappedPointer((void **)&ddata, &bytes, pgres));
        }

        checkCudaErrors(cudaMemcpy(hdata, ddata,
                                   m_numBodies*4*sizeof(float), cudaMemcpyDeviceToHost));

        if (pgres)
        {
            checkCudaErrors(cudaGraphicsUnmapResources(1, &pgres, 0));
        }
    }

    return hdata;
}

void BodySystemAcc::setArray(BodyArray array, const float *data)
{
    assert(m_bInitialized);

    m_currentRead = 0;
    m_currentWrite = 1;

    switch (array)
    {
        default:
        case BODYSYSTEM_POSITION:
            {
                if (m_bUsePBO)
                {
                    glBindBuffer(GL_ARRAY_BUFFER, m_pbo[m_currentRead]);
                    glBufferSubData(GL_ARRAY_BUFFER, 0, 4 * sizeof(float) * m_numBodies, data);

                    int size = 0;
                    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, (GLint *)&size);

                    if ((unsigned)size != 4 * (sizeof(float) * m_numBodies))
                    {
                        fprintf(stderr, "WARNING: Pixel Buffer Object download failed!n");
                    }

                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                }
                else
                {
                    if (m_bUseSysMem)
                    {
                        memcpy(m_hPos[m_currentRead], data, m_numBodies * 4 * sizeof(float));
                    }
                    else
                        checkCudaErrors(cudaMemcpy(m_deviceData.dPos[m_currentRead], data,
                                                   m_numBodies * 4 * sizeof(float),
                                                   cudaMemcpyHostToDevice));
                }
            }
            break;

        case BODYSYSTEM_VELOCITY:
            if (m_bUseSysMem)
            {
                memcpy(m_hVel, data, m_numBodies * 4 * sizeof(float));
            }
            else
                checkCudaErrors(cudaMemcpy(m_deviceData.dVel, data, m_numBodies * 4 * sizeof(float),
                                           cudaMemcpyHostToDevice));

            break;
    }
}
