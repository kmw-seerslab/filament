/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "details/Camera.h"

#include "components/TransformManager.h"

#include "details/Engine.h"

#include <filament/Exposure.h>
#include <filament/Camera.h>

#include <utils/compiler.h>
#include <utils/Panic.h>

#include <math/scalar.h>

#include <math/vec2.h>

using namespace filament::math;
using namespace utils;

namespace filament {

static constexpr const float MIN_APERTURE = 0.5f;
static constexpr const float MAX_APERTURE = 64.0f;
static constexpr const float MIN_SHUTTER_SPEED = 1.0f / 25000.0f;
static constexpr const float MAX_SHUTTER_SPEED = 60.0f;
static constexpr const float MIN_SENSITIVITY = 10.0f;
static constexpr const float MAX_SENSITIVITY = 204800.0f;

FCamera::FCamera(FEngine& engine, Entity e)
        : mEngine(engine),
          mEntity(e) {
}

void UTILS_NOINLINE FCamera::setProjection(double fovInDegrees, double aspect, double near, double far,
        Camera::Fov direction) noexcept {
    double w;
    double h;
    double s = std::tan(fovInDegrees * math::d::DEG_TO_RAD / 2.0) * near;
    if (direction == Fov::VERTICAL) {
        w = s * aspect;
        h = s;
    } else {
        w = s;
        h = s / aspect;
    }
    FCamera::setProjection(Projection::PERSPECTIVE, -w, w, -h, h, near, far);
}

void FCamera::setLensProjection(double focalLengthInMillimeters,
        double aspect, double near, double far) noexcept {
    // a 35mm camera has a 36x24mm wide frame size
    double h = (0.5 * near) * ((SENSOR_SIZE * 1000.0) / focalLengthInMillimeters);
    double w = h * aspect;
    FCamera::setProjection(Projection::PERSPECTIVE, -w, w, -h, h, near, far);
}

/*
 * All methods for setting the projection funnel through here
 */

void UTILS_NOINLINE FCamera::setCustomProjection(mat4 const& p, double near, double far) noexcept {
    setCustomProjection(p, p, near, far);
}

void UTILS_NOINLINE FCamera::setCustomProjection(mat4 const& p,
        mat4 const& c, double near, double far) noexcept {
    mProjection = p;
    mProjectionForCulling = c;
    mNear = (float)near;
    mFar = (float)far;
}

void UTILS_NOINLINE FCamera::setProjection(Camera::Projection projection,
        double left, double right,
        double bottom, double top,
        double near, double far) noexcept {

    // we make sure our preconditions are verified, using default values,
    // to avoid inconsistent states in the renderer later.
    if (UTILS_UNLIKELY(left == right ||
                       bottom == top ||
                       (projection == Projection::PERSPECTIVE && (near <= 0 || far <= near)) ||
                       (projection == Projection::ORTHO && (near == far)))) {
        PANIC_LOG("Camera preconditions not met. Using default projection.");
        left = -0.1;
        right = 0.1;
        bottom = -0.1;
        top = 0.1;
        near = 0.1;
        far = 100.0;
    }

    mat4 p;
    switch (projection) {
        case Projection::PERSPECTIVE:
            /*
             * The general perspective projection in GL convention looks like this:
             *
             * P =  2N/r-l    0      r+l/r-l        0
             *       0      2N/t-b   t+b/t-b        0
             *       0        0      F+N/N-F   2*F*N/N-F
             *       0        0        -1           0
             */
            p = mat4::frustum(left, right, bottom, top, near, far);
            mProjectionForCulling = p;

            /*
             * but we're using a far plane at infinity
             *
             * P =  2N/r-l      0    r+l/r-l        0
             *       0      2N/t-b   t+b/t-b        0
             *       0       0         -1        -2*N    <-- far at infinity
             *       0       0         -1           0
             */
            p[2][2] = -1.0f;           // lim(far->inf) = -1
            p[3][2] = -2.0f * near;    // lim(far->inf) = -2*near
            break;

        case Projection::ORTHO:
            /*
             * The general orthographic projection in GL convention looks like this:
             *
             * P =  2/r-l    0         0       - r+l/r-l
             *       0      2/t-b      0       - t+b/t-b
             *       0       0       -2/F-N    - F+N/F-N
             *       0       0         0            1
             */
            p = mat4::ortho(left, right, bottom, top, near, far);
            mProjectionForCulling = p;
            break;
    }
    mProjection = p;
    mNear = float(near);
    mFar = float(far);
}

math::mat4 FCamera::getProjectionMatrix() const noexcept {
    // This is where we transform the user clip-space (GL convention) to our virtual clip-space
    // (inverted DX convention)
    // Note that this math ends up setting the projection matrix' p33 to 0, which is where we're
    // getting back a lot of precision in the depth buffer.
    const mat4 m{ mat4::row_major_init{
            mScaling.x, 0.0, 0.0, mShiftCS.x,
            0.0, mScaling.y, 0.0, mShiftCS.y,
            0.0, 0.0, -0.5, 0.5,    // GL to inverted DX convention
            0.0, 0.0, 0.0, 1.0
    }};
    return m * mProjection;
}

math::mat4 FCamera::getCullingProjectionMatrix() const noexcept {
    // The culling projection matrix stays in the GL convention
    const mat4 m{ mat4::row_major_init{
            mScaling.x, 0.0, 0.0, mShiftCS.x,
            0.0, mScaling.y, 0.0, mShiftCS.y,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0
    }};
    return m * mProjectionForCulling;
}

void UTILS_NOINLINE FCamera::setModelMatrix(const mat4f& modelMatrix) noexcept {
    FTransformManager& transformManager = mEngine.getTransformManager();
    transformManager.setTransform(transformManager.getInstance(mEntity), modelMatrix);
}

void UTILS_NOINLINE FCamera::setModelMatrix(const mat4& modelMatrix) noexcept {
    FTransformManager& transformManager = mEngine.getTransformManager();
    transformManager.setTransform(transformManager.getInstance(mEntity), modelMatrix);
}

void FCamera::lookAt(const float3& eye, const float3& center, const float3& up) noexcept {
    FTransformManager& transformManager = mEngine.getTransformManager();
    transformManager.setTransform(transformManager.getInstance(mEntity),
            mat4::lookAt(eye, center, up));
}

mat4 FCamera::getModelMatrix() const noexcept {
    FTransformManager const& transformManager = mEngine.getTransformManager();
    return transformManager.getWorldTransformAccurate(transformManager.getInstance(mEntity));
}

mat4 UTILS_NOINLINE FCamera::getViewMatrix() const noexcept {
    return inverse(getModelMatrix());
}

Frustum FCamera::getCullingFrustum() const noexcept {
    // for culling purposes we keep the far plane where it is
    return Frustum(mat4f{ getCullingProjectionMatrix() * getViewMatrix() });
}

void FCamera::setExposure(float aperture, float shutterSpeed, float sensitivity) noexcept {
    mAperture = clamp(aperture, MIN_APERTURE, MAX_APERTURE);
    mShutterSpeed = clamp(shutterSpeed, MIN_SHUTTER_SPEED, MAX_SHUTTER_SPEED);
    mSensitivity = clamp(sensitivity, MIN_SENSITIVITY, MAX_SENSITIVITY);
}

double FCamera::getFocalLength() const noexcept {
    return (FCamera::SENSOR_SIZE * mProjection[1][1]) * 0.5;
}

double FCamera::computeEffectiveFocalLength(double focalLength, double focusDistance) noexcept {
    focusDistance = std::max(focalLength, focusDistance);
    return (focusDistance * focalLength) / (focusDistance - focalLength);
}

double FCamera::computeEffectiveFov(double fovInDegrees, double focusDistance) noexcept {
    double f = 0.5 * FCamera::SENSOR_SIZE / std::tan(fovInDegrees * math::d::DEG_TO_RAD * 0.5);
    focusDistance = std::max(f, focusDistance);
    double fov = 2.0 * std::atan(FCamera::SENSOR_SIZE * (focusDistance - f) / (2.0 * focusDistance * f));
    return fov * math::d::RAD_TO_DEG;
}

template<typename T>
math::details::TMat44<T> inverseProjection(const math::details::TMat44<T>& p) noexcept {
    math::details::TMat44<T> r;
    const T A = 1 / p[0][0];
    const T B = 1 / p[1][1];
    if (p[2][3] != T(0)) {
        // perspective projection
        // a 0 tx 0
        // 0 b ty 0
        // 0 0 tz c
        // 0 0 -1 0
        const T C = 1 / p[3][2];
        r[0][0] = A;
        r[1][1] = B;
        r[2][2] = 0;
        r[2][3] = C;
        r[3][0] = p[2][0] * A;    // not needed if symmetric
        r[3][1] = p[2][1] * B;    // not needed if symmetric
        r[3][2] = -1;
        r[3][3] = p[2][2] * C;
    } else {
        // orthographic projection
        // a 0 0 tx
        // 0 b 0 ty
        // 0 0 c tz
        // 0 0 0 1
        const T C = 1 / p[2][2];
        r[0][0] = A;
        r[1][1] = B;
        r[2][2] = C;
        r[3][3] = 1;
        r[3][0] = -p[3][0] * A;
        r[3][1] = -p[3][1] * B;
        r[3][2] = -p[3][2] * C;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------

CameraInfo::CameraInfo(FCamera const& camera) noexcept {
    projection         = mat4f{ camera.getProjectionMatrix() };
    cullingProjection  = mat4f{ camera.getCullingProjectionMatrix() };
    model              = mat4f{ camera.getModelMatrix() };
    view               = mat4f{ camera.getViewMatrix() };
    zn                 = camera.getNear();
    zf                 = camera.getCullingFar();
    ev100              = Exposure::ev100(camera);
    f                  = camera.getFocalLength();
    A                  = f / camera.getAperture();
    d                  = std::max(zn, camera.getFocusDistance());
}

CameraInfo::CameraInfo(FCamera const& camera, const math::mat4& worldOriginCamera) noexcept {
    const mat4 modelMatrix{ worldOriginCamera * camera.getModelMatrix() };
    projection         = mat4f{ camera.getProjectionMatrix() };
    cullingProjection  = mat4f{ camera.getCullingProjectionMatrix() };
    model              = mat4f{ modelMatrix };
    view               = mat4f{ inverse(modelMatrix) };
    zn                 = camera.getNear();
    zf                 = camera.getCullingFar();
    ev100              = Exposure::ev100(camera);
    f                  = camera.getFocalLength();
    A                  = f / camera.getAperture();
    d                  = std::max(zn, camera.getFocusDistance());
    worldOffset        = camera.getPosition();
    worldOrigin        = mat4f{ worldOriginCamera };
}

// ------------------------------------------------------------------------------------------------
// Trampoline calling into private implementation
// ------------------------------------------------------------------------------------------------

mat4f Camera::inverseProjection(const mat4f& p) noexcept {
    return filament::inverseProjection(p);
}
mat4 Camera::inverseProjection(const mat4 & p) noexcept {
    return filament::inverseProjection(p);
}

void Camera::setProjection(Camera::Projection projection, double left, double right, double bottom,
        double top, double near, double far) noexcept {
    upcast(this)->setProjection(projection, left, right, bottom, top, near, far);
}

void Camera::setProjection(double fovInDegrees, double aspect, double near, double far,
        Camera::Fov direction) noexcept {
    upcast(this)->setProjection(fovInDegrees, aspect, near, far, direction);
}

void Camera::setLensProjection(double focalLengthInMillimeters,
        double aspect, double near, double far) noexcept {
    upcast(this)->setLensProjection(focalLengthInMillimeters, aspect, near, far);
}

void Camera::setCustomProjection(mat4 const& projection, double near, double far) noexcept {
    upcast(this)->setCustomProjection(projection, near, far);
}

void Camera::setCustomProjection(mat4 const& projection, mat4 const& projectionForCulling,
        double near, double far) noexcept {
    upcast(this)->setCustomProjection(projection, projectionForCulling, near, far);
}

void Camera::setScaling(math::double2 scaling) noexcept {
    upcast(this)->setScaling(scaling);
}

void Camera::setShift(math::double2 shift) noexcept {
    upcast(this)->setShift(shift);
}

mat4 Camera::getProjectionMatrix() const noexcept {
    return upcast(this)->getUserProjectionMatrix();
}

mat4 Camera::getCullingProjectionMatrix() const noexcept {
    return upcast(this)->getUserCullingProjectionMatrix();
}

math::double4 Camera::getScaling() const noexcept {
    return upcast(this)->getScaling();
}

math::double2 Camera::getShift() const noexcept {
    return upcast(this)->getShift();
}

float Camera::getNear() const noexcept {
    return upcast(this)->getNear();
}

float Camera::getCullingFar() const noexcept {
    return upcast(this)->getCullingFar();
}

void Camera::setModelMatrix(const mat4& modelMatrix) noexcept {
    upcast(this)->setModelMatrix(modelMatrix);
}

void Camera::setModelMatrix(const mat4f& modelMatrix) noexcept {
    upcast(this)->setModelMatrix(modelMatrix);
}

void Camera::lookAt(const float3& eye, const float3& center, float3 const& up) noexcept {
    upcast(this)->lookAt(eye, center, up);
}

void Camera::lookAt(const float3& eye, const float3& center) noexcept {
    upcast(this)->lookAt(eye, center, {0, 1, 0});
}

mat4 Camera::getModelMatrix() const noexcept {
    return upcast(this)->getModelMatrix();
}

mat4 Camera::getViewMatrix() const noexcept {
    return upcast(this)->getViewMatrix();
}

float3 Camera::getPosition() const noexcept {
    return upcast(this)->getPosition();
}

float3 Camera::getLeftVector() const noexcept {
    return upcast(this)->getLeftVector();
}

float3 Camera::getUpVector() const noexcept {
    return upcast(this)->getUpVector();
}

float3 Camera::getForwardVector() const noexcept {
    return upcast(this)->getForwardVector();
}

float Camera::getFieldOfViewInDegrees(Camera::Fov direction) const noexcept {
    return upcast(this)->getFieldOfViewInDegrees(direction);
}

Frustum Camera::getFrustum() const noexcept {
    return upcast(this)->getCullingFrustum();
}

utils::Entity Camera::getEntity() const noexcept {
    return upcast(this)->getEntity();
}

void Camera::setExposure(float aperture, float shutterSpeed, float ISO) noexcept {
    upcast(this)->setExposure(aperture, shutterSpeed, ISO);
}

float Camera::getAperture() const noexcept {
    return upcast(this)->getAperture();
}

float Camera::getShutterSpeed() const noexcept {
    return upcast(this)->getShutterSpeed();
}

float Camera::getSensitivity() const noexcept {
    return upcast(this)->getSensitivity();
}

void Camera::setFocusDistance(float distance) noexcept {
    upcast(this)->setFocusDistance(distance);
}

float Camera::getFocusDistance() const noexcept {
    return upcast(this)->getFocusDistance();
}

double Camera::getFocalLength() const noexcept {
    return upcast(this)->getFocalLength();
}

double Camera::computeEffectiveFocalLength(double focalLength, double focusDistance) noexcept {
    return FCamera::computeEffectiveFocalLength(focalLength, focusDistance);
}

double Camera::computeEffectiveFov(double fovInDegrees, double focusDistance) noexcept {
    return FCamera::computeEffectiveFov(fovInDegrees, focusDistance);
}

} // namespace filament
