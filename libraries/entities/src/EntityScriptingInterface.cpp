//
//  EntityScriptingInterface.cpp
//  libraries/entities/src
//
//  Created by Brad Hefta-Gaub on 12/6/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <VariantMapToScriptValue.h>

#include "EntityScriptingInterface.h"
#include "EntityTree.h"
#include "LightEntityItem.h"
#include "ModelEntityItem.h"
#include "ZoneEntityItem.h"
#include "EntitiesLogging.h"


EntityScriptingInterface::EntityScriptingInterface() :
    _entityTree(NULL)
{
    auto nodeList = DependencyManager::get<NodeList>();
    connect(nodeList.data(), &NodeList::canAdjustLocksChanged, this, &EntityScriptingInterface::canAdjustLocksChanged);
    connect(nodeList.data(), &NodeList::canRezChanged, this, &EntityScriptingInterface::canRezChanged);
}

void EntityScriptingInterface::queueEntityMessage(PacketType packetType,
                                                  EntityItemID entityID, const EntityItemProperties& properties) {
    getEntityPacketSender()->queueEditEntityMessage(packetType, entityID, properties);
}

bool EntityScriptingInterface::canAdjustLocks() {
    auto nodeList = DependencyManager::get<NodeList>();
    return nodeList->getThisNodeCanAdjustLocks();
}

bool EntityScriptingInterface::canRez() {
    auto nodeList = DependencyManager::get<NodeList>();
    return nodeList->getThisNodeCanRez();
}

void EntityScriptingInterface::setEntityTree(EntityTree* modelTree) {
    if (_entityTree) {
        disconnect(_entityTree, &EntityTree::addingEntity, this, &EntityScriptingInterface::addingEntity);
        disconnect(_entityTree, &EntityTree::deletingEntity, this, &EntityScriptingInterface::deletingEntity);
        disconnect(_entityTree, &EntityTree::clearingEntities, this, &EntityScriptingInterface::clearingEntities);
    }

    _entityTree = modelTree;

    if (_entityTree) {
        connect(_entityTree, &EntityTree::addingEntity, this, &EntityScriptingInterface::addingEntity);
        connect(_entityTree, &EntityTree::deletingEntity, this, &EntityScriptingInterface::deletingEntity);
        connect(_entityTree, &EntityTree::clearingEntities, this, &EntityScriptingInterface::clearingEntities);
    }
}

void bidForSimulationOwnership(EntityItemProperties& properties) {
    // We make a bid for simulation ownership by declaring our sessionID as simulation owner 
    // in the outgoing properties.  The EntityServer may accept the bid or might not.
    auto nodeList = DependencyManager::get<NodeList>();
    const QUuid myNodeID = nodeList->getSessionUUID();
    properties.setSimulatorID(myNodeID);
}



QUuid EntityScriptingInterface::addEntity(const EntityItemProperties& properties) {

    EntityItemProperties propertiesWithSimID = properties;

    EntityItemID id = EntityItemID(QUuid::createUuid());

    // If we have a local entity tree set, then also update it.
    bool success = true;
    if (_entityTree) {
        _entityTree->lockForWrite();
        EntityItem* entity = _entityTree->addEntity(id, propertiesWithSimID);
        if (entity) {
            entity->setLastBroadcast(usecTimestampNow());
            // This Node is creating a new object.  If it's in motion, set this Node as the simulator.
            bidForSimulationOwnership(propertiesWithSimID);
        } else {
            qCDebug(entities) << "script failed to add new Entity to local Octree";
            success = false;
        }
        _entityTree->unlock();
    }

    // queue the packet
    if (success) {
        queueEntityMessage(PacketTypeEntityAdd, id, propertiesWithSimID);
    }

    return id;
}

EntityItemProperties EntityScriptingInterface::getEntityProperties(QUuid identity) {
    EntityItemProperties results;
    if (_entityTree) {
        _entityTree->lockForRead();
        EntityItem* entity = const_cast<EntityItem*>(_entityTree->findEntityByEntityItemID(EntityItemID(identity)));
        
        if (entity) {
            results = entity->getProperties();

            // TODO: improve sitting points and naturalDimensions in the future, 
            //       for now we've included the old sitting points model behavior for entity types that are models
            //        we've also added this hack for setting natural dimensions of models
            if (entity->getType() == EntityTypes::Model) {
                const FBXGeometry* geometry = _entityTree->getGeometryForEntity(entity);
                if (geometry) {
                    results.setSittingPoints(geometry->sittingPoints);
                    Extents meshExtents = geometry->getUnscaledMeshExtents();
                    results.setNaturalDimensions(meshExtents.maximum - meshExtents.minimum);
                }
            }

        }
        _entityTree->unlock();
    }
    
    return results;
}

QUuid EntityScriptingInterface::editEntity(QUuid id, const EntityItemProperties& properties) {
    EntityItemID entityID(id);
    // If we have a local entity tree set, then also update it.
    if (_entityTree) {
        _entityTree->lockForWrite();
        _entityTree->updateEntity(entityID, properties);
        _entityTree->unlock();
    }

    // make sure the properties has a type, so that the encode can know which properties to include
    if (properties.getType() == EntityTypes::Unknown) {
        EntityItem* entity = _entityTree->findEntityByEntityItemID(entityID);
        if (entity) {
            // we need to change the outgoing properties, so we make a copy, modify, and send.
            EntityItemProperties modifiedProperties = properties;
            entity->setLastBroadcast(usecTimestampNow());
            modifiedProperties.setType(entity->getType());
            bidForSimulationOwnership(modifiedProperties);
            queueEntityMessage(PacketTypeEntityEdit, entityID, modifiedProperties);
            return id;
        }
    }

    queueEntityMessage(PacketTypeEntityEdit, entityID, properties);
    return id;
}

void EntityScriptingInterface::deleteEntity(QUuid id) {
    EntityItemID entityID(id);
    bool shouldDelete = true;

    // If we have a local entity tree set, then also update it.
    if (_entityTree) {
        _entityTree->lockForWrite();

        EntityItem* entity = const_cast<EntityItem*>(_entityTree->findEntityByEntityItemID(entityID));
        if (entity) {
            if (entity->getLocked()) {
                shouldDelete = false;
            } else {
                _entityTree->deleteEntity(entityID);
            }
        }

        _entityTree->unlock();
    }

    // if at this point, we know the id, and we should still delete the entity, send the update to the entity server
    if (shouldDelete) {
        getEntityPacketSender()->queueEraseEntityMessage(entityID);
    }
}

QUuid EntityScriptingInterface::findClosestEntity(const glm::vec3& center, float radius) const {
    EntityItemID result; 
    if (_entityTree) {
        _entityTree->lockForRead();
        const EntityItem* closestEntity = _entityTree->findClosestEntity(center, radius);
        _entityTree->unlock();
        if (closestEntity) {
            result = closestEntity->getEntityItemID();
        }
    }
    return result;
}


void EntityScriptingInterface::dumpTree() const {
    if (_entityTree) {
        _entityTree->lockForRead();
        _entityTree->dumpTree();
        _entityTree->unlock();
    }
}

QVector<QUuid> EntityScriptingInterface::findEntities(const glm::vec3& center, float radius) const {
    QVector<QUuid> result;
    if (_entityTree) {
        _entityTree->lockForRead();
        QVector<const EntityItem*> entities;
        _entityTree->findEntities(center, radius, entities);
        _entityTree->unlock();
        
        foreach (const EntityItem* entity, entities) {
            result << entity->getEntityItemID();
        }
    }
    return result;
}

QVector<QUuid> EntityScriptingInterface::findEntitiesInBox(const glm::vec3& corner, const glm::vec3& dimensions) const {
    QVector<QUuid> result;
    if (_entityTree) {
        _entityTree->lockForRead();
        AABox box(corner, dimensions);
        QVector<EntityItem*> entities;
        _entityTree->findEntities(box, entities);
        _entityTree->unlock();
        
        foreach (const EntityItem* entity, entities) {
            result << entity->getEntityItemID();
        }
    }
    return result;
}

RayToEntityIntersectionResult EntityScriptingInterface::findRayIntersection(const PickRay& ray, bool precisionPicking) {
    return findRayIntersectionWorker(ray, Octree::TryLock, precisionPicking);
}

RayToEntityIntersectionResult EntityScriptingInterface::findRayIntersectionBlocking(const PickRay& ray, bool precisionPicking) {
    return findRayIntersectionWorker(ray, Octree::Lock, precisionPicking);
}

RayToEntityIntersectionResult EntityScriptingInterface::findRayIntersectionWorker(const PickRay& ray, 
                                                                                    Octree::lockType lockType, 
                                                                                    bool precisionPicking) {


    RayToEntityIntersectionResult result;
    if (_entityTree) {
        OctreeElement* element;
        EntityItem* intersectedEntity = NULL;
        result.intersects = _entityTree->findRayIntersection(ray.origin, ray.direction, element, result.distance, result.face, 
                                                                (void**)&intersectedEntity, lockType, &result.accurate, 
                                                                precisionPicking);
        if (result.intersects && intersectedEntity) {
            result.entityID = intersectedEntity->getEntityItemID();
            result.properties = intersectedEntity->getProperties();
            result.intersection = ray.origin + (ray.direction * result.distance);
        }
    }
    return result;
}

void EntityScriptingInterface::setLightsArePickable(bool value) {
    LightEntityItem::setLightsArePickable(value);
}

bool EntityScriptingInterface::getLightsArePickable() const {
    return LightEntityItem::getLightsArePickable();
}

void EntityScriptingInterface::setZonesArePickable(bool value) {
    ZoneEntityItem::setZonesArePickable(value);
}

bool EntityScriptingInterface::getZonesArePickable() const {
    return ZoneEntityItem::getZonesArePickable();
}

void EntityScriptingInterface::setDrawZoneBoundaries(bool value) {
    ZoneEntityItem::setDrawZoneBoundaries(value);
}

bool EntityScriptingInterface::getDrawZoneBoundaries() const {
    return ZoneEntityItem::getDrawZoneBoundaries();
}

void EntityScriptingInterface::setSendPhysicsUpdates(bool value) {
    EntityItem::setSendPhysicsUpdates(value);
}

bool EntityScriptingInterface::getSendPhysicsUpdates() const {
    return EntityItem::getSendPhysicsUpdates();
}


RayToEntityIntersectionResult::RayToEntityIntersectionResult() : 
    intersects(false), 
    accurate(true), // assume it's accurate
    entityID(),
    properties(),
    distance(0),
    face(),
    entity(NULL)
{ 
}

QScriptValue RayToEntityIntersectionResultToScriptValue(QScriptEngine* engine, const RayToEntityIntersectionResult& value) {
    QScriptValue obj = engine->newObject();
    obj.setProperty("intersects", value.intersects);
    obj.setProperty("accurate", value.accurate);
    QScriptValue entityItemValue = EntityItemIDtoScriptValue(engine, value.entityID);
    obj.setProperty("entityID", entityItemValue);

    QScriptValue propertiesValue = EntityItemPropertiesToScriptValue(engine, value.properties);
    obj.setProperty("properties", propertiesValue);

    obj.setProperty("distance", value.distance);

    QString faceName = "";    
    // handle BoxFace
    switch (value.face) {
        case MIN_X_FACE:
            faceName = "MIN_X_FACE";
            break;
        case MAX_X_FACE:
            faceName = "MAX_X_FACE";
            break;
        case MIN_Y_FACE:
            faceName = "MIN_Y_FACE";
            break;
        case MAX_Y_FACE:
            faceName = "MAX_Y_FACE";
            break;
        case MIN_Z_FACE:
            faceName = "MIN_Z_FACE";
            break;
        case MAX_Z_FACE:
            faceName = "MAX_Z_FACE";
            break;
        case UNKNOWN_FACE:
            faceName = "UNKNOWN_FACE";
            break;
    }
    obj.setProperty("face", faceName);

    QScriptValue intersection = vec3toScriptValue(engine, value.intersection);
    obj.setProperty("intersection", intersection);
    return obj;
}

void RayToEntityIntersectionResultFromScriptValue(const QScriptValue& object, RayToEntityIntersectionResult& value) {
    value.intersects = object.property("intersects").toVariant().toBool();
    value.accurate = object.property("accurate").toVariant().toBool();
    QScriptValue entityIDValue = object.property("entityID");
    // EntityItemIDfromScriptValue(entityIDValue, value.entityID);
    quuidFromScriptValue(entityIDValue, value.entityID);
    QScriptValue entityPropertiesValue = object.property("properties");
    if (entityPropertiesValue.isValid()) {
        EntityItemPropertiesFromScriptValue(entityPropertiesValue, value.properties);
    }
    value.distance = object.property("distance").toVariant().toFloat();

    QString faceName = object.property("face").toVariant().toString();
    if (faceName == "MIN_X_FACE") {
        value.face = MIN_X_FACE;
    } else if (faceName == "MAX_X_FACE") {
        value.face = MAX_X_FACE;
    } else if (faceName == "MIN_Y_FACE") {
        value.face = MIN_Y_FACE;
    } else if (faceName == "MAX_Y_FACE") {
        value.face = MAX_Y_FACE;
    } else if (faceName == "MIN_Z_FACE") {
        value.face = MIN_Z_FACE;
    } else {
        value.face = MAX_Z_FACE;
    };
    QScriptValue intersection = object.property("intersection");
    if (intersection.isValid()) {
        vec3FromScriptValue(intersection, value.intersection);
    }
}
