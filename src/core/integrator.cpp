
/*
    pbrt source code is Copyright(c) 1998-2015
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#include "stdafx.h"

// core/integrator.cpp*
#include "integrator.h"
#include "scene.h"
#include "interaction.h"
#include "sampling.h"
#include "parallel.h"
#include "film.h"
#include "sampler.h"
#include "integrator.h"
#include "progressreporter.h"
#include "camera.h"
#include "interaction.h"
#include "stats.h"
STAT_COUNTER("Integrator/Camera rays traced", nCameraRays);
STAT_TIMER("Time/Rendering", renderingTime);

// Integrator Method Definitions
Integrator::~Integrator() {}

// Integrator Utility Functions
Spectrum SpecularReflect(const RayDifferential &ray,
                         const SurfaceInteraction &isect,
                         const SamplerIntegrator &integrator,
                         const Scene &scene, Sampler &sampler,
                         MemoryArena &arena) {
    Vector3f wo = isect.wo, wi;
    const BSDF &bsdf = *isect.bsdf;
    Float pdf;
    const Point3f &p = isect.p;
    const Normal3f &ns = isect.shading.n;
    Spectrum f = bsdf.Sample_f(wo, &wi, sampler.Get2D(), &pdf,
                               BxDFType(BSDF_REFLECTION | BSDF_SPECULAR));
    Spectrum L = Spectrum(0.f);
    if (pdf > 0.f && !f.IsBlack() && AbsDot(wi, ns) != 0.f) {
        // Compute ray differential _rd_ for specular reflection
        RayDifferential rd = isect.SpawnRay(wi, ray.depth + 1);
        if (ray.hasDifferentials) {
            rd.hasDifferentials = true;
            rd.rxOrigin = p + isect.dpdx;
            rd.ryOrigin = p + isect.dpdy;
            // Compute differential reflected directions
            Normal3f dndx = isect.shading.dndu * isect.dudx +
                            isect.shading.dndv * isect.dvdx;
            Normal3f dndy = isect.shading.dndu * isect.dudy +
                            isect.shading.dndv * isect.dvdy;
            Vector3f dwodx = -ray.rxDirection - wo,
                     dwody = -ray.ryDirection - wo;
            Float dDNdx = Dot(dwodx, ns) + Dot(wo, dndx);
            Float dDNdy = Dot(dwody, ns) + Dot(wo, dndy);
            rd.rxDirection =
                wi - dwodx + 2.f * Vector3f(Dot(wo, ns) * dndx + dDNdx * ns);
            rd.ryDirection =
                wi - dwody + 2.f * Vector3f(Dot(wo, ns) * dndy + dDNdy * ns);
        }
        Spectrum Li = integrator.Li(rd, scene, sampler, arena);
        L = f * Li * AbsDot(wi, ns) / pdf;
    }
    return L;
}

Spectrum SpecularTransmit(const RayDifferential &ray,
                          const SurfaceInteraction &isect,
                          const SamplerIntegrator &integrator,
                          const Scene &scene, Sampler &sampler,
                          MemoryArena &arena) {
    Vector3f wo = isect.wo, wi;
    Float pdf;
    const Point3f &p = isect.p;
    const Normal3f &ns = isect.shading.n;
    const BSDF &bsdf = *isect.bsdf;
    Spectrum f = bsdf.Sample_f(wo, &wi, sampler.Get2D(), &pdf,
                               BxDFType(BSDF_TRANSMISSION | BSDF_SPECULAR));
    Spectrum L = Spectrum(0.f);
    if (pdf > 0.f && !f.IsBlack() && AbsDot(wi, ns) != 0.f) {
        // Compute ray differential _rd_ for specular transmission
        RayDifferential rd = isect.SpawnRay(wi, ray.depth + 1);
        if (ray.hasDifferentials) {
            rd.hasDifferentials = true;
            rd.rxOrigin = p + isect.dpdx;
            rd.ryOrigin = p + isect.dpdy;

            Float eta = bsdf.eta;
            Vector3f w = -wo;
            if (Dot(wo, ns) < 0) eta = 1.f / eta;

            Normal3f dndx = isect.shading.dndu * isect.dudx +
                            isect.shading.dndv * isect.dvdx;
            Normal3f dndy = isect.shading.dndu * isect.dudy +
                            isect.shading.dndv * isect.dvdy;

            Vector3f dwodx = -ray.rxDirection - wo,
                     dwody = -ray.ryDirection - wo;
            Float dDNdx = Dot(dwodx, ns) + Dot(wo, dndx);
            Float dDNdy = Dot(dwody, ns) + Dot(wo, dndy);

            Float mu = eta * Dot(w, ns) - Dot(wi, ns);
            Float dmudx =
                (eta - (eta * eta * Dot(w, ns)) / Dot(wi, ns)) * dDNdx;
            Float dmudy =
                (eta - (eta * eta * Dot(w, ns)) / Dot(wi, ns)) * dDNdy;

            rd.rxDirection =
                wi + eta * dwodx - Vector3f(mu * dndx + dmudx * ns);
            rd.ryDirection =
                wi + eta * dwody - Vector3f(mu * dndy + dmudy * ns);
        }
        Spectrum Li = integrator.Li(rd, scene, sampler, arena);
        L = f * Li * AbsDot(wi, ns) / pdf;
    }
    return L;
}

Distribution1D *ComputeLightSamplingCDF(const Scene &scene) {
    Assert(scene.lights.size() > 0);
    std::vector<Float> lightPower;
    for (const auto &light : scene.lights)
        lightPower.push_back(light->Power().y());
    return new Distribution1D(&lightPower[0], lightPower.size());
}

Spectrum UniformSampleAllLights(const Interaction &it, const Scene &scene,
                                Sampler &sampler,
                                const std::vector<int> &numLightSamples,
                                MemoryArena &arena, bool handleMedia) {
    Spectrum L(0.f);
    for (uint32_t i = 0; i < scene.lights.size(); ++i) {
        const std::shared_ptr<Light> &light = scene.lights[i];
        int nSamples = numLightSamples[i];
        const Point2f *lightSamples = sampler.Get2DArray(nSamples);
        const Point2f *shadingSamples = sampler.Get2DArray(nSamples);
        if (lightSamples == NULL || shadingSamples == NULL) {
            L += EstimateDirect(it, sampler.Get2D(), *light, sampler.Get2D(),
                                scene, sampler, arena, handleMedia);
        } else {
            // Estimate direct lighting using sample arrays
            Spectrum Ld(0.f);
            for (int j = 0; j < nSamples; ++j) {
                Ld += EstimateDirect(it, shadingSamples[j], *light,
                                     lightSamples[j], scene, sampler, arena,
                                     handleMedia);
            }
            L += Ld / nSamples;
        }
    }
    return L;
}

Spectrum UniformSampleOneLight(const Interaction &it, const Scene &scene,
                               Sampler &sampler, MemoryArena &arena,
                               bool handleMedia) {
    // Randomly choose a single light to sample, _light_
    int nLights = int(scene.lights.size());
    if (nLights == 0) return Spectrum(0.f);
    int lightNum = std::min((int)(sampler.Get1D() * nLights), nLights - 1);
    const std::shared_ptr<Light> &light = scene.lights[lightNum];
    Point2f lightSample = sampler.Get2D();
    Point2f shadingSample = sampler.Get2D();
    return (Float)nLights * EstimateDirect(it, shadingSample, *light,
                                           lightSample, scene, sampler, arena,
                                           handleMedia);
}

Spectrum EstimateDirect(const Interaction &it, const Point2f &shadingSample,
                        const Light &light, const Point2f &lightSample,
                        const Scene &scene, Sampler &sampler,
                        MemoryArena &arena, bool handleMedia, bool specular) {
    BxDFType bsdfFlags = BxDFType(BSDF_ALL & ~(specular ? 0 : BSDF_SPECULAR));
    Spectrum Ld(0.f);
    // Sample light source with multiple importance sampling
    Vector3f wi;
    Float lightPdf = 0.f, shadingPdf = 0.f;
    VisibilityTester visibility;
    Spectrum Li = light.Sample_L(it, lightSample, &wi, &lightPdf, &visibility);
    if (lightPdf > 0. && !Li.IsBlack()) {
        Spectrum f;
        if (it.IsSurfaceInteraction()) {
            // Evaluate surface reflectance for light sampling strategy
            const SurfaceInteraction &isect = (const SurfaceInteraction &)it;
            if (isect.bsdf) {
                f = isect.bsdf->f(isect.wo, wi, bsdfFlags) *
                    AbsDot(wi, isect.shading.n);
                shadingPdf = isect.bsdf->Pdf(isect.wo, wi, bsdfFlags);
            }
        } else {
            // Evaluate medium reflectance for light sampling strategy
            const MediumInteraction &mi = (const MediumInteraction &)it;
            Float phaseValue = mi.phase->p(mi.wo, wi);
            shadingPdf = phaseValue;
            f = Spectrum(phaseValue);
        }
        if (!f.IsBlack()) {
            // Add light's contribution to reflected radiance
            if (handleMedia)
                Li *= visibility.T(scene, sampler);
            else if (!visibility.Unoccluded(scene))
                Li = Spectrum(0.f);
            if (!Li.IsBlack()) {
                if (IsDeltaLight(light.flags))
                    Ld += f * Li / lightPdf;
                else {
                    Float weight = PowerHeuristic(1, lightPdf, 1, shadingPdf);
                    Ld += f * Li * (weight / lightPdf);
                }
            }
        }
    }

    // Sample material with multiple importance sampling
    if (!(IsDeltaLight(light.flags))) {
        Spectrum f;
        bool sampledSpecular = false;
        if (it.IsSurfaceInteraction()) {
            // Sample scattered direction for surface interactions
            BxDFType sampledType;
            const SurfaceInteraction &isect = (const SurfaceInteraction &)it;
            if (isect.bsdf) {
                f = isect.bsdf->Sample_f(isect.wo, &wi, shadingSample,
                                         &shadingPdf, bsdfFlags, &sampledType);
                f *= AbsDot(wi, isect.shading.n);
                sampledSpecular = sampledType & BSDF_SPECULAR;
            }
        } else {
            // Sample scattered direction for medium interactions
            const MediumInteraction &mi = (const MediumInteraction &)it;
            shadingPdf = mi.phase->Sample_p(mi.wo, &wi, shadingSample);
            f = Spectrum(shadingPdf);
        }
        if (!f.IsBlack() && shadingPdf > 0.f) {
            Float weight = 1.f;
            if (!sampledSpecular) {
                lightPdf = light.Pdf(it, wi);
                if (lightPdf == 0.f) return Ld;
                weight = PowerHeuristic(1, shadingPdf, 1, lightPdf);
            }
            // Find intersection and compute transmittance
            SurfaceInteraction lightIsect;
            Ray ray = it.SpawnRay(wi);
            Spectrum transmittance(1.f);

            bool foundSurfaceInteraction =
                handleMedia
                    ? scene.IntersectT(ray, sampler, &lightIsect,
                                       &transmittance)
                    : scene.Intersect(ray, &lightIsect);

            // Add light contribution from material sampling
            Spectrum Li(0.f);
            if (foundSurfaceInteraction) {
                if (lightIsect.primitive->GetAreaLight() == &light)
                    Li = lightIsect.Le(-wi);
            } else
                Li = light.Le(ray);
            if (!Li.IsBlack())
                Ld += f * Li * transmittance * weight / shadingPdf;
        }
    }
    return Ld;
}

// SamplerIntegrator Method Definitions
void SamplerIntegrator::Render(const Scene &scene) {
    Preprocess(scene);
    // Run parallel tasks to render the image

    // Compute number of tiles to use for parallel rendering
    Bounds2i sampleBounds = camera->film->GetSampleBounds();
    Vector2i sampleExtent = sampleBounds.Diagonal();
    const int tileSize = 16;
    Point2i nTiles((sampleExtent.x + tileSize - 1) / tileSize,
                   (sampleExtent.y + tileSize - 1) / tileSize);
    ProgressReporter reporter(nTiles.x * nTiles.y, "Rendering");
    {
        StatTimer timer(&renderingTime);
        ParallelFor([&](const Point2i tile) {
            // Render section of image corresponding to _tile_

            // Allocate _MemoryArena_ for tile
            MemoryArena arena;

            // Get sampler instance for tile
            int seed = tile.y * nTiles.x + tile.x;
            std::unique_ptr<Sampler> tileSampler = sampler->Clone(seed);

            // Compute sample bounds for tile
            int x0 = sampleBounds.pMin.x + tile.x * tileSize;
            int x1 = std::min(x0 + tileSize, sampleBounds.pMax.x);
            int y0 = sampleBounds.pMin.y + tile.y * tileSize;
            int y1 = std::min(y0 + tileSize, sampleBounds.pMax.y);
            Bounds2i tileBounds(Point2i(x0, y0), Point2i(x1, y1));

            // Get _FilmTile_ for tile
            std::unique_ptr<FilmTile> filmTile =
                camera->film->GetFilmTile(tileBounds);

            // Loop over pixels in tile to render them
            for (Point2i pixel : tileBounds) {
                tileSampler->StartPixel(pixel);
                do {
                    // Initialize _CameraSample_ for current sample
                    CameraSample cameraSample;
                    cameraSample.pFilm = (Point2f)pixel + tileSampler->Get2D();
                    cameraSample.time = tileSampler->Get1D();
                    cameraSample.pLens = tileSampler->Get2D();

                    // Generate camera ray for current sample
                    RayDifferential ray;
                    Float rayWeight =
                        camera->GenerateRayDifferential(cameraSample, &ray);
                    ray.ScaleDifferentials(
                        1.f / std::sqrt(tileSampler->samplesPerPixel));
                    ++nCameraRays;

                    // Evaluate radiance along camera ray
                    Spectrum L = 0.f;
                    if (rayWeight > 0.f)
                        L = Li(ray, scene, *tileSampler, arena);

                    // Issue warning if unexpected radiance value returned
                    if (L.HasNaNs()) {
                        Error(
                            "Not-a-number radiance value returned "
                            "for image sample.  Setting to black.");
                        L = Spectrum(0.f);
                    } else if (L.y() < -1e-5) {
                        Error(
                            "Negative luminance value, %f, returned "
                            "for image sample.  Setting to black.",
                            L.y());
                        L = Spectrum(0.f);
                    } else if (std::isinf(L.y())) {
                        Error(
                            "Infinite luminance value returned "
                            "for image sample.  Setting to black.");
                        L = Spectrum(0.f);
                    }

                    // Add camera ray's contribution to image
                    filmTile->AddSample(cameraSample.pFilm, L, rayWeight);

                    // Free _MemoryArena_ memory from computing image sample
                    // value
                    arena.Reset();
                } while (tileSampler->StartNextSample());
            }

            // Merge image tile into _Film_
            camera->film->MergeFilmTile(std::move(filmTile));
            reporter.Update();
        }, nTiles);
        reporter.Done();
    }

    // Clean up after rendering and store final image
    camera->film->WriteImage();
}
