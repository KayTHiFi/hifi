//
//  RenderableZoneEntityItem.cpp
//
//
//  Created by Clement on 4/22/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RenderableZoneEntityItem.h"

#include <DeferredLightingEffect.h>
#include <DependencyManager.h>
#include <GeometryCache.h>
#include <PerfStat.h>

EntityItem* RenderableZoneEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    return new RenderableZoneEntityItem(entityID, properties);
}

template<typename Lambda>
void RenderableZoneEntityItem::changeProperties(Lambda setNewProperties) {
    QString oldShapeURL = getCompoundShapeURL();
    glm::vec3 oldPosition = getPosition(), oldDimensions = getDimensions();
    glm::quat oldRotation = getRotation();
    
    setNewProperties();
    
    if (oldShapeURL != getCompoundShapeURL()) {
        if (_model) {
            delete _model;
        }
        
        _model = getModel();
        _needsInitialSimulation = true;
        _model->setURL(getCompoundShapeURL(), QUrl(), true, true);
    }
    if (oldPosition != getPosition() ||
        oldRotation != getRotation() ||
        oldDimensions != getDimensions()) {
        _needsInitialSimulation = true;
    }
}

bool RenderableZoneEntityItem::setProperties(const EntityItemProperties& properties) {
    bool somethingChanged = false;
    changeProperties([&]() {
        somethingChanged = this->ZoneEntityItem::setProperties(properties);
    });
    return somethingChanged;
}

int RenderableZoneEntityItem::readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                                ReadBitstreamToTreeParams& args,
                                                                EntityPropertyFlags& propertyFlags, bool overwriteLocalData) {
    int bytesRead = 0;
    changeProperties([&]() {
        bytesRead = ZoneEntityItem::readEntitySubclassDataFromBuffer(data, bytesLeftToRead,
                                                                     args, propertyFlags, overwriteLocalData);
    });
    return bytesRead;
}

Model* RenderableZoneEntityItem::getModel() {
    Model* model = new Model();
    model->setIsWireframe(true);
    model->init();
    return model;
}

void RenderableZoneEntityItem::initialSimulation() {
    _model->setScaleToFit(true, getDimensions());
    _model->setSnapModelToRegistrationPoint(true, getRegistrationPoint());
    _model->setRotation(getRotation());
    _model->setTranslation(getPosition());
    _model->simulate(0.0f);
    _needsInitialSimulation = false;
}

void RenderableZoneEntityItem::updateGeometry() {
    if (_model && !_model->isActive() && hasCompoundShapeURL()) {
        // Since we have a delayload, we need to update the geometry if it has been downloaded
        _model->setURL(getCompoundShapeURL(), QUrl(), true);
    }
    if (_model && _model->isActive() && _needsInitialSimulation) {
        initialSimulation();
    }
}

void RenderableZoneEntityItem::render(RenderArgs* args) {
    if (_drawZoneBoundaries) {
        switch (getShapeType()) {
            case SHAPE_TYPE_COMPOUND: {
                updateGeometry();
                
                if (_model && _model->isActive()) {
                    PerformanceTimer perfTimer("zone->renderCompound");
                    glPushMatrix();
                    _model->renderInScene(getLocalRenderAlpha(), args);
                    glPopMatrix();
                }
                break;
            }
            case SHAPE_TYPE_BOX:
            case SHAPE_TYPE_SPHERE: {
                PerformanceTimer perfTimer("zone->renderPrimitive");
                glm::vec3 position = getPosition();
                glm::vec3 center = getCenter();
                glm::vec3 dimensions = getDimensions();
                glm::quat rotation = getRotation();
                
                glm::vec4 DEFAULT_COLOR(1.0f, 1.0f, 1.0f, getLocalRenderAlpha());
                
                glPushMatrix(); {
                    glTranslatef(position.x, position.y, position.z);
                    glm::vec3 axis = glm::axis(rotation);
                    glRotatef(glm::degrees(glm::angle(rotation)), axis.x, axis.y, axis.z);
                    glPushMatrix(); {
                        glm::vec3 positionToCenter = center - position;
                        glTranslatef(positionToCenter.x, positionToCenter.y, positionToCenter.z);
                        glScalef(dimensions.x, dimensions.y, dimensions.z);
                        
                        auto deferredLightingEffect = DependencyManager::get<DeferredLightingEffect>();
                        
                        if (getShapeType() == SHAPE_TYPE_SPHERE) {
                            const int SLICES = 15;
                            const int STACKS = 15;
                            deferredLightingEffect->renderWireSphere(0.5f, SLICES, STACKS, DEFAULT_COLOR);
                        } else {
                            deferredLightingEffect->renderWireCube(1.0f, DEFAULT_COLOR);
                        }
                    } glPopMatrix();
                } glPopMatrix();
                break;
            }
            default:
                // Not handled
                break;
        }
    }
}

bool RenderableZoneEntityItem::contains(const glm::vec3& point) const {
    if (getShapeType() != SHAPE_TYPE_COMPOUND) {
        return EntityItem::contains(point);
    }
    const_cast<RenderableZoneEntityItem*>(this)->updateGeometry();
    
    if (_model && _model->isActive() && EntityItem::contains(point)) {
        return _model->convexHullContains(point);
    }
    
    return false;
}
