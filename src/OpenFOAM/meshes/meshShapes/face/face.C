/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2012 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "face.H"
#include "triFace.H"
#include "triPointRef.H"
#include "mathematicalConstants.H"
#include "Swap.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

const char* const Foam::face::typeName = "face";


// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

Foam::tmp<Foam::vectorField>
Foam::face::calcEdges(const pointField& points) const
{
    tmp<vectorField> tedges(new vectorField(size()));
    vectorField& edges = tedges();

    forAll(*this, i)
    {
        label ni = fcIndex(i);

        point thisPt = points[operator[](i)];
        point nextPt = points[operator[](ni)];

        vector vec(nextPt - thisPt);
        vec /= Foam::mag(vec) + VSMALL;

        edges[i] = vec;
    }

    return tedges;
}


Foam::scalar Foam::face::edgeCos
(
    const vectorField& edges,
    const label index
) const
{
    label leftEdgeI = left(index);
    label rightEdgeI = right(index);

    // note negate on left edge to get correct left-pointing edge.
    return -(edges[leftEdgeI] & edges[rightEdgeI]);
}


Foam::label Foam::face::mostConcaveAngle
(
    const pointField& points,
    const vectorField& edges,
    scalar& maxAngle
) const
{
    vector n(normal(points));

    label index = 0;
    maxAngle = -GREAT;

    forAll(edges, i)
    {
        label leftEdgeI = left(i);
        label rightEdgeI = right(i);

        vector edgeNormal = edges[rightEdgeI] ^ edges[leftEdgeI];

        scalar edgeCos = edges[leftEdgeI] & edges[rightEdgeI];
        scalar edgeAngle = acos(max(-1.0, min(1.0, edgeCos)));

        scalar angle;

        if ((edgeNormal & n) > 0)
        {
            // Concave angle.
            angle = constant::mathematical::pi + edgeAngle;
        }
        else
        {
            // Convex angle. Note '-' to take into account that rightEdge
            // and leftEdge are head-to-tail connected.
            angle = constant::mathematical::pi - edgeAngle;
        }

        if (angle > maxAngle)
        {
            maxAngle = angle;
            index = i;
        }
    }

    return index;
}


Foam::label Foam::face::split
(
    const face::splitMode mode,
    const pointField& points,
    label& triI,
    label& quadI,
    faceList& triFaces,
    faceList& quadFaces
) const
{
    label oldIndices = (triI + quadI);

    if (size() <= 2)
    {
        FatalErrorIn
        (
            "face::split"
            "(const face::splitMode, const pointField&, label&, label&"
            ", faceList&, faceList&)"
        )
            << "Serious problem: asked to split a face with < 3 vertices"
            << abort(FatalError);
    }
    if (size() == 3)
    {
        // Triangle. Just copy.
        if (mode == COUNTTRIANGLE || mode == COUNTQUAD)
        {
            triI++;
        }
        else
        {
            triFaces[triI++] = *this;
        }
    }
    else if (size() == 4)
    {
        if (mode == COUNTTRIANGLE)
        {
            triI += 2;
        }
        if (mode == COUNTQUAD)
        {
            quadI++;
        }
        else if (mode == SPLITTRIANGLE)
        {
            //  Start at point with largest internal angle.
            const vectorField edges(calcEdges(points));

            scalar minAngle;
            label startIndex = mostConcaveAngle(points, edges, minAngle);

            label nextIndex = fcIndex(startIndex);
            label splitIndex = fcIndex(nextIndex);

            // Create triangles
            face triFace(3);
            triFace[0] = operator[](startIndex);
            triFace[1] = operator[](nextIndex);
            triFace[2] = operator[](splitIndex);

            triFaces[triI++] = triFace;

            triFace[0] = operator[](splitIndex);
            triFace[1] = operator[](fcIndex(splitIndex));
            triFace[2] = operator[](startIndex);

            triFaces[triI++] = triFace;
        }
        else
        {
            quadFaces[quadI++] = *this;
        }
    }
    else
    {
        // General case. Like quad: search for largest internal angle.

        const vectorField edges(calcEdges(points));

        scalar minAngle = 1;
        label startIndex = mostConcaveAngle(points, edges, minAngle);

        scalar bisectAngle = minAngle/2;
        vector rightEdge = edges[right(startIndex)];

        //
        // Look for opposite point which as close as possible bisects angle
        //

        // split candidate starts two points away.
        label index = fcIndex(fcIndex(startIndex));

        label minIndex = index;
        scalar minDiff = constant::mathematical::pi;

        for (label i = 0; i < size() - 3; i++)
        {
            vector splitEdge
            (
                points[operator[](index)]
              - points[operator[](startIndex)]
            );
            splitEdge /= Foam::mag(splitEdge) + VSMALL;

            const scalar splitCos = splitEdge & rightEdge;
            const scalar splitAngle = acos(max(-1.0, min(1.0, splitCos)));
            const scalar angleDiff = fabs(splitAngle - bisectAngle);

            if (angleDiff < minDiff)
            {
                minDiff = angleDiff;
                minIndex = index;
            }

            // go to next candidate
            index = fcIndex(index);
        }


        // Split into two subshapes.
        //     face1: startIndex to minIndex
        //     face2: minIndex to startIndex

        // Get sizes of the two subshapes
        label diff = 0;
        if (minIndex > startIndex)
        {
            diff = minIndex - startIndex;
        }
        else
        {
            // folded around
            diff = minIndex + size() - startIndex;
        }

        label nPoints1 = diff + 1;
        label nPoints2 = size() - diff + 1;

        // Collect face1 points
        face face1(nPoints1);

        index = startIndex;
        for (label i = 0; i < nPoints1; i++)
        {
            face1[i] = operator[](index);
            index = fcIndex(index);
        }

        // Collect face2 points
        face face2(nPoints2);

        index = minIndex;
        for (label i = 0; i < nPoints2; i++)
        {
            face2[i] = operator[](index);
            index = fcIndex(index);
        }

        // Split faces
        face1.split(mode, points, triI, quadI, triFaces, quadFaces);
        face2.split(mode, points, triI, quadI, triFaces, quadFaces);
    }

    return (triI + quadI - oldIndices);
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::face::face(const triFace& f)
:
    labelList(f)
{}


// * * * * * * * * * * * * * Static Member Functions * * * * * * * * * * * * //


// return
//   0: no match
//  +1: identical
//  -1: same face, but different orientation
int Foam::face::compare(const face& a, const face& b)
{
    // Basic rule: we assume that the sequence of labels in each list
    // will be circular in the same order (but not necessarily in the
    // same direction or from the same starting point).

    // Trivial reject: faces are different size
    label sizeA = a.size();
    label sizeB = b.size();

    if (sizeA != sizeB)
    {
        return 0;
    }


    // Full list comparison
    const label firstA = a[0];
    label Bptr = -1;

    forAll(b, i)
    {
        if (b[i] == firstA)
        {
            Bptr = i;        // 'found match' at element 'i'
            break;
        }
    }

    // If no match was found, return 0
    if (Bptr < 0)
    {
        return 0;
    }

    // Now we must look for the direction, if any
    label secondA = a[1];

    if (sizeA > 1 && (secondA == firstA || firstA == a[sizeA - 1]))
    {
        face ca = a;
        ca.collapse();

        face cb = b;
        cb.collapse();

        return face::compare(ca, cb);
    }

    int dir = 0;

    // Check whether at top of list
    Bptr++;
    if (Bptr == b.size())
    {
        Bptr = 0;
    }

    // Test whether upward label matches second A label
    if (b[Bptr] == secondA)
    {
        // Yes - direction is 'up'
        dir = 1;
    }
    else
    {
        // No - so look downwards, checking whether at bottom of list
        Bptr -= 2;

        if (Bptr < 0)
        {
            // wraparound
            Bptr += b.size();
        }

        // Test whether downward label matches second A label
        if (b[Bptr] == secondA)
        {
            // Yes - direction is 'down'
            dir = -1;
        }
    }

    // Check whether a match was made at all, and exit 0 if not
    if (dir == 0)
    {
        return 0;
    }

    // Decrement size by 2 to account for first searches
    sizeA -= 2;

    // We now have both direction of search and next element
    // to search, so we can continue search until no more points.
    label Aptr = 1;
    if (dir > 0)
    {
        while (sizeA--)
        {
            Aptr++;
            if (Aptr >= a.size())
            {
                Aptr = 0;
            }

            Bptr++;
            if (Bptr >= b.size())
            {
                Bptr = 0;
            }

            if (a[Aptr] != b[Bptr])
            {
                return 0;
            }
        }
    }
    else
    {
        while (sizeA--)
        {
            Aptr++;
            if (Aptr >= a.size())
            {
                Aptr = 0;
            }

            Bptr--;
            if (Bptr < 0)
            {
                Bptr = b.size() - 1;
            }

            if (a[Aptr] != b[Bptr])
            {
                return 0;
            }
        }
    }

    // They must be equal - return direction
    return dir;
}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //


Foam::label Foam::face::collapse()
{
    if (size() > 1)
    {
        label ci = 0;
        for (label i=1; i<size(); i++)
        {
            if (operator[](i) != operator[](ci))
            {
                operator[](++ci) = operator[](i);
            }
        }

        if (operator[](ci) != operator[](0))
        {
            ci++;
        }

        setSize(ci);
    }

    return size();
}


void Foam::face::flip()
{
    const label n = size();

    if (n > 2)
    {
        for (label i=1; i < (n+1)/2; ++i)
        {
            Swap(operator[](i), operator[](n-i));
        }
    }
}


Foam::point Foam::face::centre(const pointField& points) const
{
    // Calculate the centre by breaking the face into triangles and
    // area-weighted averaging their centres

    const label nPoints = size();

    // If the face is a triangle, do a direct calculation
    if (nPoints == 3)
    {
        return
            (1.0/3.0)
           *(
               points[operator[](0)]
             + points[operator[](1)]
             + points[operator[](2)]
            );
    }


    point centrePoint = point::zero;
    for (register label pI=0; pI<nPoints; ++pI)
    {
        centrePoint += points[operator[](pI)];
    }
    centrePoint /= nPoints;

    scalar sumA = 0;
    vector sumAc = vector::zero;

    for (register label pI=0; pI<nPoints; ++pI)
    {
        const point& nextPoint = points[operator[]((pI + 1) % nPoints)];

        // Calculate 3*triangle centre
        const vector ttc
        (
            points[operator[](pI)]
          + nextPoint
          + centrePoint
        );

        // Calculate 2*triangle area
        const scalar ta = Foam::mag
        (
            (points[operator[](pI)] - centrePoint)
          ^ (nextPoint - centrePoint)
        );

        sumA += ta;
        sumAc += ta*ttc;
    }

    if (sumA > VSMALL)
    {
        return sumAc/(3.0*sumA);
    }
    else
    {
        return centrePoint;
    }
}


Foam::vector Foam::face::normal(const pointField& p) const
{
    const label nPoints = size();

    // Calculate the normal by summing the face triangle normals.
    // Changed to deal with small concavity by using a central decomposition
    //

    // If the face is a triangle, do a direct calculation to avoid round-off
    // error-related problems
    //
    if (nPoints == 3)
    {
        return triPointRef
        (
            p[operator[](0)],
            p[operator[](1)],
            p[operator[](2)]
        ).normal();
    }

    register label pI;

    point centrePoint = vector::zero;
    for (pI = 0; pI < nPoints; ++pI)
    {
        centrePoint += p[operator[](pI)];
    }
    centrePoint /= nPoints;

    vector n = vector::zero;

    point nextPoint = centrePoint;

    for (pI = 0; pI < nPoints; ++pI)
    {
        if (pI < nPoints - 1)
        {
            nextPoint = p[operator[](pI + 1)];
        }
        else
        {
            nextPoint = p[operator[](0)];
        }

        // Note: for best accuracy, centre point always comes last
        //
        n += triPointRef
        (
            p[operator[](pI)],
            nextPoint,
            centrePoint
        ).normal();
    }

    return n;
}


Foam::face Foam::face::reverseFace() const
{
    // reverse the label list and return
    // The starting points of the original and reverse face are identical.

    const labelList& f = *this;
    labelList newList(size());

    newList[0] = f[0];

    for (label pointI = 1; pointI < newList.size(); pointI++)
    {
        newList[pointI] = f[size() - pointI];
    }

    return face(xferMove(newList));
}


Foam::label Foam::face::which(const label globalIndex) const
{
    const labelList& f = *this;

    forAll(f, localIdx)
    {
        if (f[localIdx] == globalIndex)
        {
            return localIdx;
        }
    }

    return -1;
}


Foam::scalar Foam::face::sweptVol
(
    const pointField& oldPoints,
    const pointField& newPoints
) const
{
    // This Optimization causes a small discrepancy between the swept-volume of
    // opposite faces of complex cells with triangular faces opposing polygons.
    // It could be used without problem for tetrahedral cells
    // if (size() == 3)
    // {
    //     return
    //     (
    //         triPointRef
    //         (
    //             oldPoints[operator[](0)],
    //             oldPoints[operator[](1)],
    //             oldPoints[operator[](2)]
    //         ).sweptVol
    //         (
    //             triPointRef
    //             (
    //                 newPoints[operator[](0)],
    //                 newPoints[operator[](1)],
    //                 newPoints[operator[](2)]
    //             )
    //         )
    //     );
    // }

    scalar sv = 0;

    // Calculate the swept volume by breaking the face into triangles and
    // summing their swept volumes.
    // Changed to deal with small concavity by using a central decomposition

    point centreOldPoint = centre(oldPoints);
    point centreNewPoint = centre(newPoints);

    label nPoints = size();

    point nextOldPoint = centreOldPoint;
    point nextNewPoint = centreNewPoint;

    for (register label pI = 0; pI < nPoints; ++pI)
    {
        if (pI < nPoints - 1)
        {
            nextOldPoint = oldPoints[operator[](pI + 1)];
            nextNewPoint = newPoints[operator[](pI + 1)];
        }
        else
        {
            nextOldPoint = oldPoints[operator[](0)];
            nextNewPoint = newPoints[operator[](0)];
        }

        // Note: for best accuracy, centre point always comes last
        sv += triPointRef
              (
                  centreOldPoint,
                  oldPoints[operator[](pI)],
                  nextOldPoint
              ).sweptVol
              (
                  triPointRef
                  (
                      centreNewPoint,
                      newPoints[operator[](pI)],
                      nextNewPoint
                  )
              );
    }

    return sv;
}


Foam::tensor Foam::face::inertia
(
    const pointField& p,
    const point& refPt,
    scalar density
) const
{
    // If the face is a triangle, do a direct calculation
    if (size() == 3)
    {
        return triPointRef
        (
            p[operator[](0)],
            p[operator[](1)],
            p[operator[](2)]
        ).inertia(refPt, density);
    }

    const point ctr = centre(p);

    tensor J = tensor::zero;

    forAll(*this, i)
    {
        J += triPointRef
        (
            p[operator[](i)],
            p[operator[](fcIndex(i))],
            ctr
        ).inertia(refPt, density);
    }

    return J;
}


Foam::edgeList Foam::face::edges() const
{
    const labelList& points = *this;

    edgeList e(points.size());

    for (label pointI = 0; pointI < points.size() - 1; ++pointI)
    {
        e[pointI] = edge(points[pointI], points[pointI + 1]);
    }

    // add last edge
    e.last() = edge(points.last(), points[0]);

    return e;
}


int Foam::face::edgeDirection(const edge& e) const
{
    forAll(*this, i)
    {
        if (operator[](i) == e.start())
        {
            if (operator[](rcIndex(i)) == e.end())
            {
                // reverse direction
                return -1;
            }
            else if (operator[](fcIndex(i)) == e.end())
            {
                // forward direction
                return 1;
            }

            // no match
            return 0;
        }
        else if (operator[](i) == e.end())
        {
            if (operator[](rcIndex(i)) == e.start())
            {
                // forward direction
                return 1;
            }
            else if (operator[](fcIndex(i)) == e.start())
            {
                // reverse direction
                return -1;
            }

            // no match
            return 0;
        }
    }

    // not found
    return 0;
}


// Number of triangles directly known from number of vertices
Foam::label Foam::face::nTriangles(const pointField&) const
{
    return nTriangles();
}


Foam::label Foam::face::triangles
(
    const pointField& points,
    label& triI,
    faceList& triFaces
) const
{
    label quadI = 0;
    faceList quadFaces;

    return split(SPLITTRIANGLE, points, triI, quadI, triFaces, quadFaces);
}


Foam::label Foam::face::nTrianglesQuads
(
    const pointField& points,
    label& triI,
    label& quadI
) const
{
    faceList triFaces;
    faceList quadFaces;

    return split(COUNTQUAD, points, triI, quadI, triFaces, quadFaces);
}


Foam::label Foam::face::trianglesQuads
(
    const pointField& points,
    label& triI,
    label& quadI,
    faceList& triFaces,
    faceList& quadFaces
) const
{
    return split(SPLITQUAD, points, triI, quadI, triFaces, quadFaces);
}


// ************************************************************************* //
