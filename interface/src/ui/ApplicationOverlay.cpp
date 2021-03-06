//
//  ApplicationOverlay.cpp
//  interface/src/ui/overlays
//
//  Created by Benjamin Arnold on 5/27/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "InterfaceConfig.h"

#include <QOpenGLFramebufferObject>
#include <QOpenGLTexture>

#include <avatar/AvatarManager.h>
#include <GLMHelpers.h>
#include <PathUtils.h>
#include <gpu/GLBackend.h>
#include <GLMHelpers.h>
#include <PerfStat.h>
#include <OffscreenUi.h>

#include "AudioClient.h"
#include "audio/AudioIOStatsRenderer.h"
#include "audio/AudioScope.h"
#include "audio/AudioToolBox.h"
#include "Application.h"
#include "ApplicationOverlay.h"
#include "devices/CameraToolBox.h"

#include "Util.h"
#include "ui/Stats.h"

// Used to animate the magnification windows
const float MAG_SPEED = 0.08f;

const quint64 MSECS_TO_USECS = 1000ULL;

const float WHITE_TEXT[] = { 0.93f, 0.93f, 0.93f };
const float RETICLE_COLOR[] = { 0.0f, 198.0f / 255.0f, 244.0f / 255.0f };
const float reticleSize = TWO_PI / 100.0f;


const float CONNECTION_STATUS_BORDER_COLOR[] = { 1.0f, 0.0f, 0.0f };
const float CONNECTION_STATUS_BORDER_LINE_WIDTH = 4.0f;

static const float MOUSE_PITCH_RANGE = 1.0f * PI;
static const float MOUSE_YAW_RANGE = 0.5f * TWO_PI;
static const glm::vec2 MOUSE_RANGE(MOUSE_YAW_RANGE, MOUSE_PITCH_RANGE);

// Return a point's cartesian coordinates on a sphere from pitch and yaw
glm::vec3 getPoint(float yaw, float pitch) {
    return glm::vec3(glm::cos(-pitch) * (-glm::sin(yaw)),
                     glm::sin(-pitch),
                     glm::cos(-pitch) * (-glm::cos(yaw)));
}

//Checks if the given ray intersects the sphere at the origin. result will store a multiplier that should
//be multiplied by dir and added to origin to get the location of the collision
bool raySphereIntersect(const glm::vec3 &dir, const glm::vec3 &origin, float r, float* result)
{
    //Source: http://wiki.cgsociety.org/index.php/Ray_Sphere_Intersection
    
    //Compute A, B and C coefficients
    float a = glm::dot(dir, dir);
    float b = 2 * glm::dot(dir, origin);
    float c = glm::dot(origin, origin) - (r * r);
    
    //Find discriminant
    float disc = b * b - 4 * a * c;
    
    // if discriminant is negative there are no real roots, so return
    // false as ray misses sphere
    if (disc < 0) {
        return false;
    }
    
    // compute q as described above
    float distSqrt = sqrtf(disc);
    float q;
    if (b < 0) {
        q = (-b - distSqrt) / 2.0;
    } else {
        q = (-b + distSqrt) / 2.0;
    }
    
    // compute t0 and t1
    float t0 = q / a;
    float t1 = c / q;
    
    // make sure t0 is smaller than t1
    if (t0 > t1) {
        // if t0 is bigger than t1 swap them around
        float temp = t0;
        t0 = t1;
        t1 = temp;
    }
    
    // if t1 is less than zero, the object is in the ray's negative direction
    // and consequently the ray misses the sphere
    if (t1 < 0) {
        return false;
    }
    
    // if t0 is less than zero, the intersection point is at t1
    if (t0 < 0) {
        *result = t1;
        return true;
    } else { // else the intersection point is at t0
        *result = t0;
        return true;
    }
}

void ApplicationOverlay::renderReticle(glm::quat orientation, float alpha) {
    glPushMatrix(); {
        glm::vec3 axis = glm::axis(orientation);
        glRotatef(glm::degrees(glm::angle(orientation)), axis.x, axis.y, axis.z);
        glm::vec3 topLeft = getPoint(reticleSize / 2.0f, -reticleSize / 2.0f);
        glm::vec3 topRight = getPoint(-reticleSize / 2.0f, -reticleSize / 2.0f);
        glm::vec3 bottomLeft = getPoint(reticleSize / 2.0f, reticleSize / 2.0f);
        glm::vec3 bottomRight = getPoint(-reticleSize / 2.0f, reticleSize / 2.0f);
        
        // TODO: this version of renderQuad() needs to take a color
        glm::vec4 reticleColor = { RETICLE_COLOR[0], RETICLE_COLOR[1], RETICLE_COLOR[2], alpha };
        
        
        
        DependencyManager::get<GeometryCache>()->renderQuad(topLeft, bottomLeft, bottomRight, topRight,
                                                            glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f),
                                                            glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f), 
                                                            reticleColor, _reticleQuad);
    } glPopMatrix();
}

ApplicationOverlay::ApplicationOverlay() :
    _textureFov(glm::radians(DEFAULT_HMD_UI_ANGULAR_SIZE)),
    _textureAspectRatio(1.0f),
    _lastMouseMove(0),
    _magnifier(true),
    _alpha(1.0f),
    _oculusUIRadius(1.0f),
    _trailingAudioLoudness(0.0f),
    _crosshairTexture(0),
    _previousBorderWidth(-1),
    _previousBorderHeight(-1),
    _previousMagnifierBottomLeft(),
    _previousMagnifierBottomRight(),
    _previousMagnifierTopLeft(),
    _previousMagnifierTopRight()
{
    memset(_reticleActive, 0, sizeof(_reticleActive));
    memset(_magActive, 0, sizeof(_reticleActive));
    memset(_magSizeMult, 0, sizeof(_magSizeMult));

    auto geometryCache = DependencyManager::get<GeometryCache>();
    
    _reticleQuad = geometryCache->allocateID();
    _magnifierQuad = geometryCache->allocateID();
    _audioRedQuad = geometryCache->allocateID();
    _audioGreenQuad = geometryCache->allocateID();
    _audioBlueQuad = geometryCache->allocateID();
    _domainStatusBorder = geometryCache->allocateID();
    _magnifierBorder = geometryCache->allocateID();
    
    // Once we move UI rendering and screen rendering to different
    // threads, we need to use a sync object to deteremine when
    // the current UI texture is no longer being read from, and only 
    // then release it back to the UI for re-use
    auto offscreenUi = DependencyManager::get<OffscreenUi>();
    connect(offscreenUi.data(), &OffscreenUi::textureUpdated, this, [&](GLuint textureId) {
        auto offscreenUi = DependencyManager::get<OffscreenUi>();
        offscreenUi->lockTexture(textureId);
        assert(!glGetError());
        std::swap(_newUiTexture, textureId);
        if (textureId) {
            offscreenUi->releaseTexture(textureId);
        }
    });
}

ApplicationOverlay::~ApplicationOverlay() {
}

// Renders the overlays either to a texture or to the screen
void ApplicationOverlay::renderOverlay() {
    PerformanceWarning warn(Menu::getInstance()->isOptionChecked(MenuOption::PipelineWarnings), "ApplicationOverlay::displayOverlay()");
    Overlays& overlays = qApp->getOverlays();
    
    _textureFov = glm::radians(_hmdUIAngularSize);
    glm::vec2 size = qApp->getCanvasSize();
    _textureAspectRatio = aspect(size);

    //Handle fading and deactivation/activation of UI
    
    // Render 2D overlay
    glMatrixMode(GL_PROJECTION);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    _overlays.buildFramebufferObject();
    _overlays.bind();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, size.x, size.y);

    glPushMatrix(); {
        const float NEAR_CLIP = -10000;
        const float FAR_CLIP = 10000;
        glLoadIdentity();
        glOrtho(0, size.x, size.y, 0, NEAR_CLIP, FAR_CLIP);

        glMatrixMode(GL_MODELVIEW);

        renderAudioMeter();
        renderCameraToggle();

        renderStatsAndLogs();

        // give external parties a change to hook in
        emit qApp->renderingOverlay();

        overlays.renderHUD();

        renderPointers();

        renderDomainConnectionStatusBorder();

        glMatrixMode(GL_PROJECTION);
    } glPopMatrix();

    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_ALPHA, GL_ONE);

    _overlays.release();
}

// A quick and dirty solution for compositing the old overlay 
// texture with the new one
template <typename F>
void with_each_texture(GLuint firstPassTexture, GLuint secondPassTexture, F f) {
    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    if (firstPassTexture) {
        glBindTexture(GL_TEXTURE_2D, firstPassTexture);
        f();
    }
    if (secondPassTexture) {
        glBindTexture(GL_TEXTURE_2D, secondPassTexture);
        f();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

// Draws the FBO texture for the screen
void ApplicationOverlay::displayOverlayTexture() {
    if (_alpha == 0.0f) {
        return;
    }
    glMatrixMode(GL_PROJECTION);
    glPushMatrix(); {
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        if (_alpha < 1.0) {
            glEnable(GL_BLEND);
        }
        glViewport(0, 0, qApp->getDeviceSize().width(), qApp->getDeviceSize().height());

        static const glm::vec2 topLeft(-1, 1);
        static const glm::vec2 bottomRight(1, -1);
        static const glm::vec2 texCoordTopLeft(0.0f, 1.0f);
        static const glm::vec2 texCoordBottomRight(1.0f, 0.0f);
        with_each_texture(_overlays.getTexture(), _newUiTexture, [&] {
            DependencyManager::get<GeometryCache>()->renderQuad(topLeft, bottomRight, texCoordTopLeft, texCoordBottomRight,
                glm::vec4(1.0f, 1.0f, 1.0f, _alpha));
        });
    } glPopMatrix();
}

// Draws the FBO texture for Oculus rift.
void ApplicationOverlay::displayOverlayTextureHmd(Camera& whichCamera) {
    if (_alpha == 0.0f) {
        return;
    }

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_ALPHA, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_LIGHTING);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.01f);


    //Update and draw the magnifiers
    MyAvatar* myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    const glm::quat& orientation = myAvatar->getOrientation();
    // Always display the HMD overlay relative to the camera position but 
    // remove the HMD pose offset.  This results in an overlay that sticks with you 
    // even in third person mode, but isn't drawn at a fixed distance.
    glm::vec3 position = whichCamera.getPosition();
    position -= qApp->getCamera()->getHmdPosition();
    const float scale = myAvatar->getScale() * _oculusUIRadius;
    
//    glm::vec3 eyeOffset = setEyeOffsetPosition;
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix(); {
        glTranslatef(position.x, position.y, position.z);
        glm::mat4 rotation = glm::toMat4(orientation);
        glMultMatrixf(&rotation[0][0]);
        glScalef(scale, scale, scale);
        for (int i = 0; i < NUMBER_OF_RETICLES; i++) {
            
            if (_magActive[i]) {
                _magSizeMult[i] += MAG_SPEED;
                if (_magSizeMult[i] > 1.0f) {
                    _magSizeMult[i] = 1.0f;
                }
            } else {
                _magSizeMult[i] -= MAG_SPEED;
                if (_magSizeMult[i] < 0.0f) {
                    _magSizeMult[i] = 0.0f;
                }
            }
            
            if (_magSizeMult[i] > 0.0f) {
                //Render magnifier, but dont show border for mouse magnifier
                glm::vec2 projection = screenToOverlay(glm::vec2(_reticlePosition[MOUSE].x(),
                                                                 _reticlePosition[MOUSE].y()));
                with_each_texture(_overlays.getTexture(), 0, [&] {
                    renderMagnifier(projection, _magSizeMult[i], i != MOUSE);
                });
            }
        }
        
        glDepthMask(GL_FALSE);
        glDisable(GL_ALPHA_TEST);
        
        static float textureFOV = 0.0f, textureAspectRatio = 1.0f;
        if (textureFOV != _textureFov ||
            textureAspectRatio != _textureAspectRatio) {
            textureFOV = _textureFov;
            textureAspectRatio = _textureAspectRatio;
            
            _overlays.buildVBO(_textureFov, _textureAspectRatio, 80, 80);
        }

        with_each_texture(_overlays.getTexture(), _newUiTexture, [&] {
            _overlays.render();
        });

        if (!Application::getInstance()->isMouseHidden()) {
            renderPointersOculus(myAvatar->getDefaultEyePosition());
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_TEXTURE_2D);
        
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_ALPHA, GL_ONE);
        glEnable(GL_LIGHTING);
    } glPopMatrix();
}

// Draws the FBO texture for 3DTV.
void ApplicationOverlay::displayOverlayTextureStereo(Camera& whichCamera, float aspectRatio, float fov) {
    if (_alpha == 0.0f) {
        return;
    }
    
    MyAvatar* myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    const glm::vec3& viewMatrixTranslation = qApp->getViewMatrixTranslation();
    
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_ALPHA, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    
    glMatrixMode(GL_MODELVIEW);
    
    glPushMatrix();
    glLoadIdentity();
    // Transform to world space
    glm::quat rotation = whichCamera.getRotation();
    glm::vec3 axis2 = glm::axis(rotation);
    glRotatef(-glm::degrees(glm::angle(rotation)), axis2.x, axis2.y, axis2.z);
    glTranslatef(viewMatrixTranslation.x, viewMatrixTranslation.y, viewMatrixTranslation.z);
    
    // Translate to the front of the camera
    glm::vec3 pos = whichCamera.getPosition();
    glm::quat rot = myAvatar->getOrientation();
    glm::vec3 axis = glm::axis(rot);
    
    glTranslatef(pos.x, pos.y, pos.z);
    glRotatef(glm::degrees(glm::angle(rot)), axis.x, axis.y, axis.z);
    
    glm::vec4 overlayColor = {1.0f, 1.0f, 1.0f, _alpha};
    
    //Render
    const GLfloat distance = 1.0f;
    
    const GLfloat halfQuadHeight = distance * tan(fov);
    const GLfloat halfQuadWidth = halfQuadHeight * aspectRatio;
    const GLfloat quadWidth = halfQuadWidth * 2.0f;
    const GLfloat quadHeight = halfQuadHeight * 2.0f;
    
    GLfloat x = -halfQuadWidth;
    GLfloat y = -halfQuadHeight;
    glDisable(GL_DEPTH_TEST);

    with_each_texture(_overlays.getTexture(), _newUiTexture, [&] {
        DependencyManager::get<GeometryCache>()->renderQuad(glm::vec3(x, y + quadHeight, -distance),
                                                glm::vec3(x + quadWidth, y + quadHeight, -distance),
                                                glm::vec3(x + quadWidth, y, -distance),
                                                glm::vec3(x, y, -distance),
                                                glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 1.0f), 
                                                glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 0.0f),
                                                overlayColor);
    });
    
    if (!_crosshairTexture) {
        _crosshairTexture = DependencyManager::get<TextureCache>()->
            getImageTexture(PathUtils::resourcesPath() + "images/sixense-reticle.png");
    }
    
    //draw the mouse pointer
    glBindTexture(GL_TEXTURE_2D, gpu::GLBackend::getTextureID(_crosshairTexture));
    glm::vec2 canvasSize = qApp->getCanvasSize();
    const float reticleSize = 40.0f / canvasSize.x * quadWidth;
    x -= reticleSize / 2.0f;
    y += reticleSize / 2.0f;
    const float mouseX = (qApp->getMouseX() / (float)canvasSize.x) * quadWidth;
    const float mouseY = (1.0 - (qApp->getMouseY() / (float)canvasSize.y)) * quadHeight;
    
    glm::vec4 reticleColor = { RETICLE_COLOR[0], RETICLE_COLOR[1], RETICLE_COLOR[2], 1.0f };

    DependencyManager::get<GeometryCache>()->renderQuad(glm::vec3(x + mouseX, y + mouseY, -distance), 
                                                glm::vec3(x + mouseX + reticleSize, y + mouseY, -distance),
                                                glm::vec3(x + mouseX + reticleSize, y + mouseY - reticleSize, -distance),
                                                glm::vec3(x + mouseX, y + mouseY - reticleSize, -distance),
                                                glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), 
                                                glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f),
                                                reticleColor, _reticleQuad);

    glEnable(GL_DEPTH_TEST);
    
    glPopMatrix();
    
    glDepthMask(GL_TRUE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_ALPHA, GL_ONE);
    glEnable(GL_LIGHTING);
}

void ApplicationOverlay::computeHmdPickRay(glm::vec2 cursorPos, glm::vec3& origin, glm::vec3& direction) const {
    cursorPos *= qApp->getCanvasSize();
    const glm::vec2 projection = screenToSpherical(cursorPos);
    // The overlay space orientation of the mouse coordinates
    const glm::quat orientation(glm::vec3(-projection.y, projection.x, 0.0f));
    // FIXME We now have the direction of the ray FROM THE DEFAULT HEAD POSE.  
    // Now we need to account for the actual camera position relative to the overlay
    glm::vec3 overlaySpaceDirection = glm::normalize(orientation * IDENTITY_FRONT);


    const glm::vec3& hmdPosition = qApp->getCamera()->getHmdPosition();
    const glm::quat& hmdOrientation = qApp->getCamera()->getHmdRotation();

    // We need the RAW camera orientation and position, because this is what the overlay is
    // rendered relative to
    const glm::vec3 overlayPosition = qApp->getCamera()->getPosition() - hmdPosition;
    const glm::quat overlayOrientation = qApp->getCamera()->getRotation() * glm::inverse(hmdOrientation);

    // Intersection UI overlay space
    glm::vec3 worldSpaceDirection = overlayOrientation * overlaySpaceDirection;
    glm::vec3 intersectionWithUi = glm::normalize(worldSpaceDirection) * _oculusUIRadius;
    intersectionWithUi += overlayPosition;

    // Intersection in world space
    origin = overlayPosition + hmdPosition;
    direction = glm::normalize(intersectionWithUi - origin);
}

//Caculate the click location using one of the sixense controllers. Scale is not applied
QPoint ApplicationOverlay::getPalmClickLocation(const PalmData *palm) const {
    MyAvatar* myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();

    glm::vec3 tip = myAvatar->getLaserPointerTipPosition(palm);
    glm::vec3 eyePos = myAvatar->getHead()->getEyePosition();
    glm::quat invOrientation = glm::inverse(myAvatar->getOrientation());
    //direction of ray goes towards camera
    glm::vec3 dir = invOrientation * glm::normalize(qApp->getCamera()->getPosition() - tip);
    glm::vec3 tipPos = invOrientation * (tip - eyePos);

    QPoint rv;
    auto canvasSize = qApp->getCanvasSize();
    if (qApp->isHMDMode()) {
        float t;

        //We back the ray up by dir to ensure that it will not start inside the UI.
        glm::vec3 adjustedPos = tipPos - dir;
        //Find intersection of crosshair ray. 
        if (raySphereIntersect(dir, adjustedPos, _oculusUIRadius * myAvatar->getScale(), &t)){
            glm::vec3 collisionPos = adjustedPos + dir * t;
            //Normalize it in case its not a radius of 1
            collisionPos = glm::normalize(collisionPos);
            //If we hit the back hemisphere, mark it as not a collision
            if (collisionPos.z > 0) {
                rv.setX(INT_MAX);
                rv.setY(INT_MAX);
            } else {

                float u = asin(collisionPos.x) / (_textureFov)+0.5f;
                float v = 1.0 - (asin(collisionPos.y) / (_textureFov)+0.5f);

                rv.setX(u * canvasSize.x);
                rv.setY(v * canvasSize.y);
            }
        } else {
            //if they did not click on the overlay, just set the coords to INT_MAX
            rv.setX(INT_MAX);
            rv.setY(INT_MAX);
        }
    } else {
        glm::dmat4 projection;
        qApp->getProjectionMatrix(&projection);

        glm::vec4 clipSpacePos = glm::vec4(projection * glm::dvec4(tipPos, 1.0));
        glm::vec3 ndcSpacePos;
        if (clipSpacePos.w != 0) {
            ndcSpacePos = glm::vec3(clipSpacePos) / clipSpacePos.w;
        }

        rv.setX(((ndcSpacePos.x + 1.0) / 2.0) * canvasSize.x);
        rv.setY((1.0 - ((ndcSpacePos.y + 1.0) / 2.0)) * canvasSize.y);
    }
    return rv;
}

//Finds the collision point of a world space ray
bool ApplicationOverlay::calculateRayUICollisionPoint(const glm::vec3& position, const glm::vec3& direction, glm::vec3& result) const {
    MyAvatar* myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    
    glm::quat inverseOrientation = glm::inverse(myAvatar->getOrientation());

    glm::vec3 relativePosition = inverseOrientation * (position - myAvatar->getDefaultEyePosition());
    glm::vec3 relativeDirection = glm::normalize(inverseOrientation * direction);

    float t;
    if (raySphereIntersect(relativeDirection, relativePosition, _oculusUIRadius * myAvatar->getScale(), &t)){
        result = position + direction * t;
        return true;
    }

    return false;
}

//Renders optional pointers
void ApplicationOverlay::renderPointers() {
    //lazily load crosshair texture
    if (_crosshairTexture == 0) {
        _crosshairTexture = DependencyManager::get<TextureCache>()->
            getImageTexture(PathUtils::resourcesPath() + "images/sixense-reticle.png");
    }
    glEnable(GL_TEXTURE_2D);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gpu::GLBackend::getTextureID(_crosshairTexture));

    if (qApp->isHMDMode() && !qApp->getLastMouseMoveWasSimulated() && !qApp->isMouseHidden()) {
        //If we are in oculus, render reticle later
        if (_lastMouseMove == 0) {
            _lastMouseMove = usecTimestampNow();
        }
        QPoint position = QPoint(qApp->getTrueMouseX(), qApp->getTrueMouseY());
        
        static const int MAX_IDLE_TIME = 3;
        if (_reticlePosition[MOUSE] != position) {
            _lastMouseMove = usecTimestampNow();
        } else if (usecTimestampNow() - _lastMouseMove > MAX_IDLE_TIME * USECS_PER_SECOND) {
            //float pitch = 0.0f, yaw = 0.0f, roll = 0.0f; // radians
            //OculusManager::getEulerAngles(yaw, pitch, roll);
            glm::quat orientation = qApp->getHeadOrientation(); // (glm::vec3(pitch, yaw, roll));
            glm::vec3 result;
            
            MyAvatar* myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
            if (calculateRayUICollisionPoint(myAvatar->getEyePosition(),
                                             myAvatar->getOrientation() * orientation * IDENTITY_FRONT,
                                             result)) {
                glm::vec3 lookAtDirection = glm::inverse(myAvatar->getOrientation()) * (result - myAvatar->getDefaultEyePosition());
                glm::vec2 spericalPos = directionToSpherical(glm::normalize(lookAtDirection));
                glm::vec2 screenPos = sphericalToScreen(spericalPos);
                position = QPoint(screenPos.x, screenPos.y);
                // FIXME
                //glCanvas->cursor().setPos(glCanvas->mapToGlobal(position));
            } else {
                qDebug() << "No collision point";
            }
        }
        
        _reticlePosition[MOUSE] = position;
        _reticleActive[MOUSE] = true;
        _magActive[MOUSE] = _magnifier;
        _reticleActive[LEFT_CONTROLLER] = false;
        _reticleActive[RIGHT_CONTROLLER] = false;
    } else if (qApp->getLastMouseMoveWasSimulated() && Menu::getInstance()->isOptionChecked(MenuOption::SixenseMouseInput)) {
        _lastMouseMove = 0;
        //only render controller pointer if we aren't already rendering a mouse pointer
        _reticleActive[MOUSE] = false;
        _magActive[MOUSE] = false;
        renderControllerPointers();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

void ApplicationOverlay::renderControllerPointers() {
    MyAvatar* myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();

    //Static variables used for storing controller state
    static quint64 pressedTime[NUMBER_OF_RETICLES] = { 0ULL, 0ULL, 0ULL };
    static bool isPressed[NUMBER_OF_RETICLES] = { false, false, false };
    static bool stateWhenPressed[NUMBER_OF_RETICLES] = { false, false, false };

    const HandData* handData = DependencyManager::get<AvatarManager>()->getMyAvatar()->getHandData();

    for (unsigned int palmIndex = 2; palmIndex < 4; palmIndex++) {
        const int index = palmIndex - 1;

        const PalmData* palmData = NULL;

        if (palmIndex >= handData->getPalms().size()) {
            return;
        }

        if (handData->getPalms()[palmIndex].isActive()) {
            palmData = &handData->getPalms()[palmIndex];
        } else {
            continue;
        }

        int controllerButtons = palmData->getControllerButtons();

        //Check for if we should toggle or drag the magnification window
        if (controllerButtons & BUTTON_3) {
            if (isPressed[index] == false) {
                //We are now dragging the window
                isPressed[index] = true;
                //set the pressed time in us
                pressedTime[index] = usecTimestampNow();
                stateWhenPressed[index] = _magActive[index];
            }
        } else if (isPressed[index]) {
            isPressed[index] = false;
            //If the button was only pressed for < 250 ms
            //then disable it.

            const int MAX_BUTTON_PRESS_TIME = 250 * MSECS_TO_USECS;
            if (usecTimestampNow() < pressedTime[index] + MAX_BUTTON_PRESS_TIME) {
                _magActive[index] = !stateWhenPressed[index];
            }
        }

        //if we have the oculus, we should make the cursor smaller since it will be
        //magnified
        if (qApp->isHMDMode()) {

            QPoint point = getPalmClickLocation(palmData);

            _reticlePosition[index] = point;

            //When button 2 is pressed we drag the mag window
            if (isPressed[index]) {
                _magActive[index] = true;
            }

            // If oculus is enabled, we draw the crosshairs later
            continue;
        }

        auto canvasSize = qApp->getCanvasSize();
        int mouseX, mouseY;
        if (Menu::getInstance()->isOptionChecked(MenuOption::SixenseLasers)) {
            QPoint res = getPalmClickLocation(palmData);
            mouseX = res.x();
            mouseY = res.y();
        } else {
            // Get directon relative to avatar orientation
            glm::vec3 direction = glm::inverse(myAvatar->getOrientation()) * palmData->getFingerDirection();

            // Get the angles, scaled between (-0.5,0.5)
            float xAngle = (atan2(direction.z, direction.x) + M_PI_2);
            float yAngle = 0.5f - ((atan2(direction.z, direction.y) + M_PI_2));

            // Get the pixel range over which the xAngle and yAngle are scaled
            float cursorRange = canvasSize.x * SixenseManager::getInstance().getCursorPixelRangeMult();

            mouseX = (canvasSize.x / 2.0f + cursorRange * xAngle);
            mouseY = (canvasSize.y / 2.0f + cursorRange * yAngle);
        }

        //If the cursor is out of the screen then don't render it
        if (mouseX < 0 || mouseX >= (int)canvasSize.x || mouseY < 0 || mouseY >= (int)canvasSize.y) {
            _reticleActive[index] = false;
            continue;
        }
        _reticleActive[index] = true;


        const float reticleSize = 40.0f;

        mouseX -= reticleSize / 2.0f;
        mouseY += reticleSize / 2.0f;


        glm::vec2 topLeft(mouseX, mouseY);
        glm::vec2 bottomRight(mouseX + reticleSize, mouseY - reticleSize);
        glm::vec2 texCoordTopLeft(0.0f, 0.0f);
        glm::vec2 texCoordBottomRight(1.0f, 1.0f);

        DependencyManager::get<GeometryCache>()->renderQuad(topLeft, bottomRight, texCoordTopLeft, texCoordBottomRight,
                                                            glm::vec4(RETICLE_COLOR[0], RETICLE_COLOR[1], RETICLE_COLOR[2], 1.0f));
        
    }
}

void ApplicationOverlay::renderPointersOculus(const glm::vec3& eyePos) {
    glBindTexture(GL_TEXTURE_2D, gpu::GLBackend::getTextureID(_crosshairTexture));
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_MODELVIEW);
    
    //Controller Pointers
    MyAvatar* myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    for (int i = 0; i < (int)myAvatar->getHand()->getNumPalms(); i++) {

        PalmData& palm = myAvatar->getHand()->getPalms()[i];
        if (palm.isActive()) {
            glm::vec3 tip = myAvatar->getLaserPointerTipPosition(&palm);
            glm::vec3 tipDirection = glm::normalize(glm::inverse(myAvatar->getOrientation()) * (tip - eyePos));
            float pitch = -glm::asin(tipDirection.y);
            float yawSign = glm::sign(-tipDirection.x);
            float yaw = glm::acos(-tipDirection.z) *
                        ((yawSign == 0.0f) ? 1.0f : yawSign);
            glm::quat orientation = glm::quat(glm::vec3(pitch, yaw, 0.0f));
            renderReticle(orientation, _alpha);
        } 
    }

    //Mouse Pointer
    if (_reticleActive[MOUSE]) {
        glm::vec2 projection = screenToSpherical(glm::vec2(_reticlePosition[MOUSE].x(),
                                                           _reticlePosition[MOUSE].y()));
        glm::quat orientation(glm::vec3(-projection.y, projection.x, 0.0f));
        renderReticle(orientation, _alpha);
    }

    glEnable(GL_DEPTH_TEST);
}

//Renders a small magnification of the currently bound texture at the coordinates
void ApplicationOverlay::renderMagnifier(glm::vec2 magPos, float sizeMult, bool showBorder) {
    if (!_magnifier) {
        return;
    }
    auto canvasSize = qApp->getCanvasSize();
    
    const int widgetWidth = canvasSize.x;
    const int widgetHeight = canvasSize.y;
    
    const float halfWidth = (MAGNIFY_WIDTH / _textureAspectRatio) * sizeMult / 2.0f;
    const float halfHeight = MAGNIFY_HEIGHT * sizeMult / 2.0f;
    // Magnification Texture Coordinates
    const float magnifyULeft = (magPos.x - halfWidth) / (float)widgetWidth;
    const float magnifyURight = (magPos.x + halfWidth) / (float)widgetWidth;
    const float magnifyVTop = 1.0f - (magPos.y - halfHeight) / (float)widgetHeight;
    const float magnifyVBottom = 1.0f - (magPos.y + halfHeight) / (float)widgetHeight;
    
    const float newHalfWidth = halfWidth * MAGNIFY_MULT;
    const float newHalfHeight = halfHeight * MAGNIFY_MULT;
    //Get yaw / pitch value for the corners
    const glm::vec2 topLeftYawPitch = overlayToSpherical(glm::vec2(magPos.x - newHalfWidth,
                                                                   magPos.y - newHalfHeight));
    const glm::vec2 bottomRightYawPitch = overlayToSpherical(glm::vec2(magPos.x + newHalfWidth,
                                                                       magPos.y + newHalfHeight));
    
    const glm::vec3 bottomLeft = getPoint(topLeftYawPitch.x, bottomRightYawPitch.y);
    const glm::vec3 bottomRight = getPoint(bottomRightYawPitch.x, bottomRightYawPitch.y);
    const glm::vec3 topLeft = getPoint(topLeftYawPitch.x, topLeftYawPitch.y);
    const glm::vec3 topRight = getPoint(bottomRightYawPitch.x, topLeftYawPitch.y);

    auto geometryCache = DependencyManager::get<GeometryCache>();

    if (bottomLeft != _previousMagnifierBottomLeft || bottomRight != _previousMagnifierBottomRight
        || topLeft != _previousMagnifierTopLeft || topRight != _previousMagnifierTopRight) {
        QVector<glm::vec3> border;
        border << topLeft;
        border << bottomLeft;
        border << bottomRight;
        border << topRight;
        border << topLeft;
        geometryCache->updateVertices(_magnifierBorder, border, glm::vec4(1.0f, 0.0f, 0.0f, _alpha));

        _previousMagnifierBottomLeft = bottomLeft;
        _previousMagnifierBottomRight = bottomRight;
        _previousMagnifierTopLeft = topLeft;
        _previousMagnifierTopRight = topRight;
    }
    
    glPushMatrix(); {
        if (showBorder) {
            glDisable(GL_TEXTURE_2D);
            glLineWidth(1.0f);
            //Outer Line
            geometryCache->renderVertices(gpu::LINE_STRIP, _magnifierBorder);
            glEnable(GL_TEXTURE_2D);
        }
        glm::vec4 magnifierColor = { 1.0f, 1.0f, 1.0f, _alpha };

        DependencyManager::get<GeometryCache>()->renderQuad(bottomLeft, bottomRight, topRight, topLeft,
                                                    glm::vec2(magnifyULeft, magnifyVBottom), 
                                                    glm::vec2(magnifyURight, magnifyVBottom), 
                                                    glm::vec2(magnifyURight, magnifyVTop), 
                                                    glm::vec2(magnifyULeft, magnifyVTop),
                                                    magnifierColor, _magnifierQuad);
        
    } glPopMatrix();
}

const int AUDIO_METER_GAP = 5;
const int MUTE_ICON_PADDING = 10;

void ApplicationOverlay::renderCameraToggle() {
    if (Menu::getInstance()->isOptionChecked(MenuOption::NoFaceTracking)) {
        return;
    }

    int audioMeterY;
    bool smallMirrorVisible = Menu::getInstance()->isOptionChecked(MenuOption::Mirror) && !qApp->isHMDMode();
    bool boxed = smallMirrorVisible &&
        !Menu::getInstance()->isOptionChecked(MenuOption::FullscreenMirror);
    if (boxed) {
        audioMeterY = MIRROR_VIEW_HEIGHT + AUDIO_METER_GAP + MUTE_ICON_PADDING;
    } else {
        audioMeterY = AUDIO_METER_GAP + MUTE_ICON_PADDING;
    }

    DependencyManager::get<CameraToolBox>()->render(MIRROR_VIEW_LEFT_PADDING + AUDIO_METER_GAP, audioMeterY, boxed);
}

void ApplicationOverlay::renderAudioMeter() {
    auto audio = DependencyManager::get<AudioClient>();

    //  Audio VU Meter and Mute Icon
    const int MUTE_ICON_SIZE = 24;
    const int AUDIO_METER_HEIGHT = 8;
    const int INTER_ICON_GAP = 2;

    int cameraSpace = 0;
    int audioMeterWidth = MIRROR_VIEW_WIDTH - MUTE_ICON_SIZE - MUTE_ICON_PADDING;
    int audioMeterScaleWidth = audioMeterWidth - 2;
    int audioMeterX = MIRROR_VIEW_LEFT_PADDING + MUTE_ICON_SIZE + AUDIO_METER_GAP;
    if (!Menu::getInstance()->isOptionChecked(MenuOption::NoFaceTracking)) {
        cameraSpace = MUTE_ICON_SIZE + INTER_ICON_GAP;
        audioMeterWidth -= cameraSpace;
        audioMeterScaleWidth -= cameraSpace;
        audioMeterX += cameraSpace;
    }

    int audioMeterY;
    bool smallMirrorVisible = Menu::getInstance()->isOptionChecked(MenuOption::Mirror) && !qApp->isHMDMode();
    bool boxed = smallMirrorVisible &&
        !Menu::getInstance()->isOptionChecked(MenuOption::FullscreenMirror);
    if (boxed) {
        audioMeterY = MIRROR_VIEW_HEIGHT + AUDIO_METER_GAP + MUTE_ICON_PADDING;
    } else {
        audioMeterY = AUDIO_METER_GAP + MUTE_ICON_PADDING;
    }

    const glm::vec4 AUDIO_METER_BLUE = { 0.0, 0.0, 1.0, 1.0 };
    const glm::vec4 AUDIO_METER_GREEN = { 0.0, 1.0, 0.0, 1.0 };
    const glm::vec4 AUDIO_METER_RED = { 1.0, 0.0, 0.0, 1.0 };
    const float CLIPPING_INDICATOR_TIME = 1.0f;
    const float AUDIO_METER_AVERAGING = 0.5;
    const float LOG2 = log(2.0f);
    const float METER_LOUDNESS_SCALE = 2.8f / 5.0f;
    const float LOG2_LOUDNESS_FLOOR = 11.0f;
    float audioGreenStart = 0.25f * audioMeterScaleWidth;
    float audioRedStart = 0.8f * audioMeterScaleWidth;
    float audioLevel = 0.0f;
    float loudness = audio->getLastInputLoudness() + 1.0f;

    _trailingAudioLoudness = AUDIO_METER_AVERAGING * _trailingAudioLoudness + (1.0f - AUDIO_METER_AVERAGING) * loudness;
    float log2loudness = log(_trailingAudioLoudness) / LOG2;

    if (log2loudness <= LOG2_LOUDNESS_FLOOR) {
        audioLevel = (log2loudness / LOG2_LOUDNESS_FLOOR) * METER_LOUDNESS_SCALE * audioMeterScaleWidth;
    } else {
        audioLevel = (log2loudness - (LOG2_LOUDNESS_FLOOR - 1.0f)) * METER_LOUDNESS_SCALE * audioMeterScaleWidth;
    }
    if (audioLevel > audioMeterScaleWidth) {
        audioLevel = audioMeterScaleWidth;
    }
    bool isClipping = ((audio->getTimeSinceLastClip() > 0.0f) && (audio->getTimeSinceLastClip() < CLIPPING_INDICATOR_TIME));

    DependencyManager::get<AudioToolBox>()->render(MIRROR_VIEW_LEFT_PADDING + AUDIO_METER_GAP, audioMeterY, cameraSpace, boxed);
    
    auto canvasSize = qApp->getCanvasSize();
    DependencyManager::get<AudioScope>()->render(canvasSize.x, canvasSize.y);
    DependencyManager::get<AudioIOStatsRenderer>()->render(WHITE_TEXT, canvasSize.x, canvasSize.y);

    audioMeterY += AUDIO_METER_HEIGHT;

    //  Draw audio meter background Quad
    DependencyManager::get<GeometryCache>()->renderQuad(audioMeterX, audioMeterY, audioMeterWidth, AUDIO_METER_HEIGHT,
                                                            glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

    if (audioLevel > audioRedStart) {
        glm::vec4 quadColor;
        if (!isClipping) {
            quadColor = AUDIO_METER_RED;
        } else {
            quadColor = glm::vec4(1, 1, 1, 1);
        }
        // Draw Red Quad
        DependencyManager::get<GeometryCache>()->renderQuad(audioMeterX + audioRedStart,
                                                            audioMeterY, 
                                                            audioLevel - audioRedStart, 
                                                            AUDIO_METER_HEIGHT, quadColor,
                                                            _audioRedQuad);
        
        audioLevel = audioRedStart;
    }

    if (audioLevel > audioGreenStart) {
        glm::vec4 quadColor;
        if (!isClipping) {
            quadColor = AUDIO_METER_GREEN;
        } else {
            quadColor = glm::vec4(1, 1, 1, 1);
        }
        // Draw Green Quad
        DependencyManager::get<GeometryCache>()->renderQuad(audioMeterX + audioGreenStart,
                                                            audioMeterY, 
                                                            audioLevel - audioGreenStart, 
                                                            AUDIO_METER_HEIGHT, quadColor,
                                                            _audioGreenQuad);

        audioLevel = audioGreenStart;
    }

    if (audioLevel >= 0) {
        glm::vec4 quadColor;
        if (!isClipping) {
            quadColor = AUDIO_METER_BLUE;
        } else {
            quadColor = glm::vec4(1, 1, 1, 1);
        }
        // Draw Blue (low level) quad
        DependencyManager::get<GeometryCache>()->renderQuad(audioMeterX,
                                                            audioMeterY,
                                                            audioLevel, AUDIO_METER_HEIGHT, quadColor,
                                                            _audioBlueQuad);
    }
}

void ApplicationOverlay::renderStatsAndLogs() {
    Application* application = Application::getInstance();
    QSharedPointer<BandwidthRecorder> bandwidthRecorder = DependencyManager::get<BandwidthRecorder>();
    
    const OctreePacketProcessor& octreePacketProcessor = application->getOctreePacketProcessor();
    NodeBounds& nodeBoundsDisplay = application->getNodeBoundsDisplay();

    //  Display stats and log text onscreen
    glLineWidth(1.0f);
    glPointSize(1.0f);

    // Determine whether to compute timing details
    bool shouldDisplayTimingDetail = Menu::getInstance()->isOptionChecked(MenuOption::DisplayDebugTimingDetails) &&
                                     Menu::getInstance()->isOptionChecked(MenuOption::Stats) &&
                                     Stats::getInstance()->isExpanded();
    if (shouldDisplayTimingDetail != PerformanceTimer::isActive()) {
        PerformanceTimer::setActive(shouldDisplayTimingDetail);
    }
    
    if (Menu::getInstance()->isOptionChecked(MenuOption::Stats)) {
        // let's set horizontal offset to give stats some margin to mirror
        int horizontalOffset = MIRROR_VIEW_WIDTH + MIRROR_VIEW_LEFT_PADDING * 2;
        int voxelPacketsToProcess = octreePacketProcessor.packetsToProcessCount();
        //  Onscreen text about position, servers, etc
        Stats::getInstance()->display(WHITE_TEXT, horizontalOffset, application->getFps(),
                                      bandwidthRecorder->getCachedTotalAverageInputPacketsPerSecond(),
                                      bandwidthRecorder->getCachedTotalAverageOutputPacketsPerSecond(),
                                      bandwidthRecorder->getCachedTotalAverageInputKilobitsPerSecond(),
                                      bandwidthRecorder->getCachedTotalAverageOutputKilobitsPerSecond(),
                                      voxelPacketsToProcess);
    }

    //  Show on-screen msec timer
    if (Menu::getInstance()->isOptionChecked(MenuOption::FrameTimer)) {
        auto canvasSize = qApp->getCanvasSize();
        quint64 mSecsNow = floor(usecTimestampNow() / 1000.0 + 0.5);
        QString frameTimer = QString("%1\n").arg((int)(mSecsNow % 1000));
        int timerBottom =
            (Menu::getInstance()->isOptionChecked(MenuOption::Stats))
            ? 80 : 20;
        drawText(canvasSize.x - 100, canvasSize.y - timerBottom,
            0.30f, 0.0f, 0, frameTimer.toUtf8().constData(), WHITE_TEXT);
    }
    nodeBoundsDisplay.drawOverlay();
}

void ApplicationOverlay::renderDomainConnectionStatusBorder() {
    auto nodeList = DependencyManager::get<NodeList>();

    if (nodeList && !nodeList->getDomainHandler().isConnected()) {
        auto geometryCache = DependencyManager::get<GeometryCache>();
        auto canvasSize = qApp->getCanvasSize();
        if ((int)canvasSize.x != _previousBorderWidth || (int)canvasSize.y != _previousBorderHeight) {
            glm::vec4 color(CONNECTION_STATUS_BORDER_COLOR[0],
                            CONNECTION_STATUS_BORDER_COLOR[1],
                            CONNECTION_STATUS_BORDER_COLOR[2], 1.0f);

            QVector<glm::vec2> border;
            border << glm::vec2(0, 0);
            border << glm::vec2(0, canvasSize.y);
            border << glm::vec2(canvasSize.x, canvasSize.y);
            border << glm::vec2(canvasSize.x, 0);
            border << glm::vec2(0, 0);
            geometryCache->updateVertices(_domainStatusBorder, border, color);
            _previousBorderWidth = canvasSize.x;
            _previousBorderHeight = canvasSize.y;
        }

        glLineWidth(CONNECTION_STATUS_BORDER_LINE_WIDTH);

        geometryCache->renderVertices(gpu::LINE_STRIP, _domainStatusBorder);
    }
}

ApplicationOverlay::TexturedHemisphere::TexturedHemisphere() :
    _vertices(0),
    _indices(0),
    _framebufferObject(NULL),
    _vbo(0, 0) {
}

ApplicationOverlay::TexturedHemisphere::~TexturedHemisphere() {
    cleanupVBO();
    if (_framebufferObject != NULL) {
        delete _framebufferObject;
    }
}

void ApplicationOverlay::TexturedHemisphere::bind() {
    _framebufferObject->bind();
}

void ApplicationOverlay::TexturedHemisphere::release() {
    _framebufferObject->release();
}

void ApplicationOverlay::TexturedHemisphere::buildVBO(const float fov,
                                                      const float aspectRatio,
                                                      const int slices,
                                                      const int stacks) {
    if (fov >= PI) {
        qDebug() << "TexturedHemisphere::buildVBO(): FOV greater or equal than Pi will create issues";
    }
    // Cleanup old VBO if necessary
    cleanupVBO();
    
    //UV mapping source: http://www.mvps.org/directx/articles/spheremap.htm
    
    // Compute number of vertices needed
    _vertices = slices * stacks;
    
    // Compute vertices positions and texture UV coordinate
    TextureVertex* vertexData = new TextureVertex[_vertices];
    TextureVertex* vertexPtr = &vertexData[0];
    for (int i = 0; i < stacks; i++) {
        float stacksRatio = (float)i / (float)(stacks - 1); // First stack is 0.0f, last stack is 1.0f
        // abs(theta) <= fov / 2.0f
        float pitch = -fov * (stacksRatio - 0.5f);
        
        for (int j = 0; j < slices; j++) {
            float slicesRatio = (float)j / (float)(slices - 1); // First slice is 0.0f, last slice is 1.0f
            // abs(phi) <= fov * aspectRatio / 2.0f
            float yaw = -fov * aspectRatio * (slicesRatio - 0.5f);
            
            vertexPtr->position = getPoint(yaw, pitch);
            vertexPtr->uv.x = slicesRatio;
            vertexPtr->uv.y = stacksRatio;
            vertexPtr++;
        }
    }
    // Create and write to buffer
    glGenBuffers(1, &_vbo.first);
    glBindBuffer(GL_ARRAY_BUFFER, _vbo.first);
    static const int BYTES_PER_VERTEX = sizeof(TextureVertex);
    glBufferData(GL_ARRAY_BUFFER, _vertices * BYTES_PER_VERTEX, vertexData, GL_STATIC_DRAW);
    delete[] vertexData;
    
    
    // Compute number of indices needed
    static const int VERTEX_PER_TRANGLE = 3;
    static const int TRIANGLE_PER_RECTANGLE = 2;
    int numberOfRectangles = (slices - 1) * (stacks - 1);
    _indices = numberOfRectangles * TRIANGLE_PER_RECTANGLE * VERTEX_PER_TRANGLE;
    
    // Compute indices order
    GLushort* indexData = new GLushort[_indices];
    GLushort* indexPtr = indexData;
    for (int i = 0; i < stacks - 1; i++) {
        for (int j = 0; j < slices - 1; j++) {
            GLushort bottomLeftIndex = i * slices + j;
            GLushort bottomRightIndex = bottomLeftIndex + 1;
            GLushort topLeftIndex = bottomLeftIndex + slices;
            GLushort topRightIndex = topLeftIndex + 1;
            
            *(indexPtr++) = topLeftIndex;
            *(indexPtr++) = bottomLeftIndex;
            *(indexPtr++) = topRightIndex;
            
            *(indexPtr++) = topRightIndex;
            *(indexPtr++) = bottomLeftIndex;
            *(indexPtr++) = bottomRightIndex;
        }
    }
    // Create and write to buffer
    glGenBuffers(1, &_vbo.second);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vbo.second);
    static const int BYTES_PER_INDEX = sizeof(GLushort);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, _indices * BYTES_PER_INDEX, indexData, GL_STATIC_DRAW);
    delete[] indexData;
}

void ApplicationOverlay::TexturedHemisphere::cleanupVBO() {
    if (_vbo.first != 0) {
        glDeleteBuffers(1, &_vbo.first);
        _vbo.first = 0;
    }
    if (_vbo.second != 0) {
        glDeleteBuffers(1, &_vbo.second);
        _vbo.second = 0;
    }
}

void ApplicationOverlay::TexturedHemisphere::buildFramebufferObject() {
    auto canvasSize = qApp->getCanvasSize();
    QSize fboSize = QSize(canvasSize.x, canvasSize.y);
    if (_framebufferObject != NULL && fboSize == _framebufferObject->size()) {
        // Already build
        return;
    }
    
    if (_framebufferObject != NULL) {
        delete _framebufferObject;
    }
    
    _framebufferObject = new QOpenGLFramebufferObject(fboSize, QOpenGLFramebufferObject::Depth);
    glBindTexture(GL_TEXTURE_2D, getTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    GLfloat borderColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glBindTexture(GL_TEXTURE_2D, 0);
}

//Renders a hemisphere with texture coordinates.
void ApplicationOverlay::TexturedHemisphere::render() {
    if (_framebufferObject == NULL || _vbo.first == 0 || _vbo.second == 0) {
        qDebug() << "TexturedHemisphere::render(): Incorrect initialisation";
        return;
    }
    
    glBindBuffer(GL_ARRAY_BUFFER, _vbo.first);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _vbo.second);
    
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    
    static const int STRIDE = sizeof(TextureVertex);
    static const void* VERTEX_POINTER = 0;
    static const void* TEX_COORD_POINTER = (void*)sizeof(glm::vec3);
    glVertexPointer(3, GL_FLOAT, STRIDE, VERTEX_POINTER);
    glTexCoordPointer(2, GL_FLOAT, STRIDE, TEX_COORD_POINTER);
    
    glDrawRangeElements(GL_TRIANGLES, 0, _vertices - 1, _indices, GL_UNSIGNED_SHORT, 0);
    
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

GLuint ApplicationOverlay::TexturedHemisphere::getTexture() {
    return _framebufferObject->texture();
}

glm::vec2 ApplicationOverlay::directionToSpherical(const glm::vec3& direction) {
    glm::vec2 result;
    // Compute yaw
    glm::vec3 normalProjection = glm::normalize(glm::vec3(direction.x, 0.0f, direction.z));
    result.x = glm::acos(glm::dot(IDENTITY_FRONT, normalProjection));
    if (glm::dot(IDENTITY_RIGHT, normalProjection) > 0.0f) {
        result.x = -glm::abs(result.x);
    } else {
        result.x = glm::abs(result.x);
    }
    // Compute pitch
    result.y = angleBetween(IDENTITY_UP, direction) - PI_OVER_TWO;
    
    return result;
}

glm::vec3 ApplicationOverlay::sphericalToDirection(const glm::vec2& sphericalPos) {
    glm::quat rotation(glm::vec3(sphericalPos.y, sphericalPos.x, 0.0f));
    return rotation * IDENTITY_FRONT;
}

glm::vec2 ApplicationOverlay::screenToSpherical(const glm::vec2& screenPos) {
    auto screenSize = qApp->getCanvasSize();
    glm::vec2 result;
    result.x = -(screenPos.x / screenSize.x - 0.5f);
    result.y = (screenPos.y / screenSize.y - 0.5f);
    result.x *= MOUSE_YAW_RANGE;
    result.y *= MOUSE_PITCH_RANGE;
    
    return result;
}

glm::vec2 ApplicationOverlay::sphericalToScreen(const glm::vec2& sphericalPos) {
    glm::vec2 result = sphericalPos;
    result.x *= -1.0;
    result /= MOUSE_RANGE;
    result += 0.5f;
    result *= qApp->getCanvasSize();
    return result; 
}

glm::vec2 ApplicationOverlay::sphericalToOverlay(const glm::vec2&  sphericalPos) const {
    glm::vec2 result = sphericalPos;
    result.x *= -1.0;
    result /= _textureFov;
    result.x /= _textureAspectRatio;
    result += 0.5f;
    result *= qApp->getCanvasSize();
    return result;
}

glm::vec2 ApplicationOverlay::overlayToSpherical(const glm::vec2&  overlayPos) const {
    glm::vec2 result = overlayPos;
    result /= qApp->getCanvasSize();
    result -= 0.5f;
    result *= _textureFov; 
    result.x *= _textureAspectRatio;
    result.x *= -1.0f;
    return result;
}

glm::vec2 ApplicationOverlay::screenToOverlay(const glm::vec2& screenPos) const {
    return sphericalToOverlay(screenToSpherical(screenPos));
}

glm::vec2 ApplicationOverlay::overlayToScreen(const glm::vec2& overlayPos) const {
    return sphericalToScreen(overlayToSpherical(overlayPos));
}
