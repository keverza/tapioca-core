// @name        Geometry
// @description Points and polylines both ways, and the two things a script is
// @description deliberately not allowed to make.
//
// A point is { x, y, z }. A polyline or polygon is an array of those. Both cross
// in either direction, so a script can read geometry the graph made and hand
// back geometry of its own.
//
// What a script CANNOT do is fabricate a mesh or an Archicad element. A mesh
// rebuilt from whatever a script left in a variable is how a graph acquires
// geometry with no normals that fails far downstream in the renderer; an element
// is a reference to something in the model, which a script must not be able to
// invent. Both are refused by name, with the node that should do it instead.
//
// @in  center : point                "Center"
// @in  radius : number = 5           "Radius"
// @in  segments : integer = 12       "Segments"
// @out ring : polyline               "Ring"
// @out first : point                 "First point"
// @out span : number                 "Circumference"

const points = [];
for (let i = 0; i < segments; i++) {
  const angle = (i / segments) * Math.PI * 2;
  points.push({
    x: center.x + Math.cos(angle) * radius,
    y: center.y + Math.sin(angle) * radius,
    z: center.z,
  });
}

ring = points;
first = points[0];
span = 2 * Math.PI * radius;
