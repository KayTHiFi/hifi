//
//  ShapeInfoUtil.cpp
//  libraries/physcis/src
//
//  Created by Andrew Meadows 2014.12.01
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <SharedUtil.h> // for MILLIMETERS_PER_METER

#include "ShapeInfoUtil.h"
#include "BulletUtil.h"



btConvexHullShape* ShapeInfoUtil::createConvexHull(const QVector<glm::vec3>& points) {
    assert(points.size() > 0);

    btConvexHullShape* hull = new btConvexHullShape();
    glm::vec3 center = points[0];
    glm::vec3 maxCorner = center;
    glm::vec3 minCorner = center;
    for (int i = 1; i < points.size(); i++) {
        center += points[i];
        maxCorner = glm::max(maxCorner, points[i]);
        minCorner = glm::min(minCorner, points[i]);
    }
    center /= (float)(points.size());

    float margin = hull->getMargin();

    // Bullet puts "margins" around all the collision shapes.  This can cause objects that use ConvexHull shapes
    // to have visible gaps between them and the surface they touch.  One option is to reduce the size of the margin
    // but this can reduce the performance and stability of the simulation (e.g. the GJK algorithm will fail to provide
    // nearest contact points and narrow-phase collisions will fall into more expensive code paths).  Alternatively
    // one can shift the geometry of the shape to make the margin surface approximately close to the visible surface.
    // This is the strategy we try, but if the object is too small then we start to reduce the margin down to some minimum.

    const float MIN_MARGIN = 0.01f;
    glm::vec3 diagonal = maxCorner - minCorner;
    float minDimension = glm::min(diagonal[0], diagonal[1]);
    minDimension = glm::min(minDimension, diagonal[2]);
    margin = glm::min(glm::max(0.5f * minDimension, MIN_MARGIN), margin);
    hull->setMargin(margin);

    // add the points, correcting for margin
    glm::vec3 relativeScale = (diagonal - glm::vec3(2.0f * margin)) / diagonal;
    glm::vec3 correctedPoint;
    for (int i = 0; i < points.size(); ++i) {
        correctedPoint = (points[i] - center) * relativeScale + center;
        hull->addPoint(btVector3(correctedPoint[0], correctedPoint[1], correctedPoint[2]), false);
    }
    hull->recalcLocalAabb();
    return hull;
}

btCollisionShape* ShapeInfoUtil::createShapeFromInfo(const ShapeInfo& info) {
    btCollisionShape* shape = NULL;
    switch(info.getType()) {
        case SHAPE_TYPE_BOX: {
            shape = new btBoxShape(glmToBullet(info.getHalfExtents()));
        }
        break;
        case SHAPE_TYPE_SPHERE: {
            float radius = info.getHalfExtents().x;
            shape = new btSphereShape(radius);
        }
        break;
        case SHAPE_TYPE_CAPSULE_Y: {
            glm::vec3 halfExtents = info.getHalfExtents();
            float radius = halfExtents.x;
            float height = 2.0f * halfExtents.y;
            shape = new btCapsuleShape(radius, height);
        }
        break;
        case SHAPE_TYPE_COMPOUND: {
            const QVector<QVector<glm::vec3>>& points = info.getPoints();
            uint32_t numSubShapes = info.getNumSubShapes();
            if (numSubShapes == 1) {
                shape = createConvexHull(info.getPoints()[0]);
            } else {
                assert(numSubShapes > 1);
                auto compound = new btCompoundShape();
                btTransform trans;
                trans.setIdentity();
                foreach (QVector<glm::vec3> hullPoints, points) {
                    btConvexHullShape* hull = createConvexHull(hullPoints);
                    compound->addChildShape (trans, hull);
                }
                shape = compound;
            }
        }
        break;
    }
    return shape;
}
