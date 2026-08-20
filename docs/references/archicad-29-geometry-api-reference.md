# Archicad 29 Geometry Extraction API — Reference

**Sources:** `AddOn/reference/archicad29-api-devkit/Support/Inc/` (C API),
`Support/Modules/GSModelDevLib/` (ModelerAPI C++),
`Support/Modules/Brep/` (low-level BREP)

## Repository Disposition

- Authority: the local AC29 API DevKit headers and ModelerAPI/BREP module sources named above.
- Conclusion: implementation reference for verified geometry extraction shapes; the request-shaped API and native contract specifications remain authoritative for the public surface.

---

## 1. Overview

Two API layers exist for extracting geometry:

| Layer | Entry Point | Route |
|-------|------------|-------|
| **C API** (`ModelAccess`) | `ACAPI_ModelAccess_GetComponent()` | `API_Component3D` union → `API_BodyType` → `API_PgonType` → `API_PedgType`/`API_EdgeType` → `API_VertType`/`API_VectType` |
| **C++ ModelerAPI** | `ACAPI_Sight_GetSelectedSightModel()` | `ModelerAPI::Model` → `ModelerAPI::Element` → `ModelerAPI::MeshBody` / `ModelerAPI::NurbsBody` |

**Entry functions** (how you get a model in the first place):

```c
// C   — switches the active 3D sight, then you call GetComponent:
GSErrCode ACAPI_Sight_Switch3DSight (API_3DSightID newSight);

// C++ — returns a ModelerAPI::Model for the selected sight:
GSErrCode ACAPI_Sight_GetSelectedSightModel (ModelerAPI::Model& model);

// C   — build a model from specific elements (separate components):
GSErrCode ACAPI_ModelAccess_GenerateModelWithSeparateComponents (const GS::Array<API_Guid>& elementGuids);

// C   — get body indices for a single element:
GSErrCode ACAPI_ModelAccess_Get3DInfo (const API_Elem_Head& elemHead, API_ElemInfo3D* info3D);
```

---

## 2. C API — ModelAccess (`Support/Inc/APIdefs_3D.h`, `ACAPinc.h`)

### 2.1 `API_3D_Head` — Common header

```c
struct API_3D_Head {
    API_3DTypeID  typeID;     // one of API_Zombie3DID, API_BodyID, API_PgonID, API_PedgID, API_EdgeID, API_VertID, API_VectID, API_UmatID, API_LghtID
    Int32         index;      // database index (1-based for subcomponents)
    Int32         elemIndex;  // internal element index + 1 (output, for body)
    Int32         bodyIndex;  // internal body index + 1    (output, for body)
};
```

### 2.2 `API_3DTypeID` — Component type enum

```c
typedef enum {
    API_Zombie3DID = 0,
    API_BodyID,
    API_PgonID,
    API_PedgID,
    API_EdgeID,
    API_VertID,
    API_VectID,
    API_UmatID,
    API_LghtID
} API_3DTypeID;
```

### 2.3 `API_BodyType` — 3D Body

```c
// Status bit flags:
#define APIBody_MulRtxt   0x0100  // more textures referenced by its materials
#define APIBody_MulColor  0x0200  // more colors referenced by its edges
#define APIBody_MulMater  0x0400  // more materials referenced by its polygons
#define APIBody_Closed    0x0001  // its geometry is closed
#define APIBody_Curved    0x0002  // it has smooth polygons

struct API_BodyType {
    API_3D_Head    head;
    API_Elem_Head  parent;        // floorplan element the body was converted from
    Int32          status;        // bitwise-OR of APIBody_* flags
    short          color;         // default color of body edges
    short          filler_0[3];
    Int32          iumat;         // default material of body polygons
    float          xmin, ymin, zmin;  // bounding box min
    float          xmax, ymax, zmax;  // bounding box max
    Int32          nPgon;         // number of polygons
    Int32          nPedg;         // number of edge references
    Int32          nEdge;         // number of edges
    Int32          nVert;         // number of vertices
    Int32          nVect;         // number of normal vectors
    Int32          filler_1;
    API_Tranmat    tranmat;       // base transformation matrix
};
```

### 2.4 `API_PgonType` — Polygon

```c
// Status bit flags:
#define APIPgon_Invis   0x0001  // invisible polygon
#define APIPgon_Curved  0x0002  // polygon of a curved surface
#define APIPgon_Concav  0x0010  // concave polygon
#define APIPgon_PHole   0x0020  // polygon with holes
#define APIPgon_HolesCnv 0x0040 // hole(s) are convex
#define APIPgon_Complex 0x0030  // the polygon is concave or with holes (Concav|PHole)

typedef struct {
    API_3D_Head  head;
    Int32        status;   // bitwise-OR of APIPgon_* flags
    Int32        iumat;    // material index
    Int32        irtxt;    // internal use
    Int32        ivect;    // normal vector index (negative = opposite direction)
    Int32        fpedg;    // index of first polygon contour edge
    Int32        lpedg;    // index of last polygon contour edge
} API_PgonType;
```

### 2.5 `API_PedgType` — Edge reference

```c
struct API_PedgType {
    API_3D_Head  head;
    Int32        pedg;     // edge index. Negative = opposite direction. Zero = contour end (hole follows).
    Int32        filler_1;
};
```

### 2.6 `API_EdgeType` — 3D Edge

```c
// Status bit flags:
#define APIEdge_Invis   0x0001  // invisible edge
#define APIEdge_Curved  0x0002  // edge of a curved surface

struct API_EdgeType {
    API_3D_Head  head;
    Int32        status;  // bitwise-OR of APIEdge_* flags
    short        filler_1;
    short        color;   // edge color
    Int32        vert1;   // vertex index of one endpoint
    Int32        vert2;   // vertex index of the other endpoint
    Int32        pgon1;   // index of one neighbouring polygon (-1 if none)
    Int32        pgon2;   // index of the other neighbouring polygon (-1 if none)
};
```

### 2.7 `API_VertType` — 3D Vertex

```c
struct API_VertType {
    API_3D_Head  head;
    double       x, y, z;  // coordinates in model space (meters)
};
```

### 2.8 `API_VectType` — 3D Normal Vector

```c
struct API_VectType {
    API_3D_Head  head;
    double       x, y, z;  // normal vector components
};
```

### 2.9 `API_LghtType` — 3D Light

```c
#define APILight_CastShadow  0x0001

typedef enum {
    APILght_DistantID,
    APILght_DirectionID,
    APILght_SpotID,
    APILght_PointID,
    APILght_SunID,
    APILght_EyeID
} API_LghtSouID;

struct API_LghtType {
    API_3D_Head     head;
    API_LghtSouID   type;
    Int32           status;
    Int32           filler_1[2];
    API_RGBColor    lightRGB;
    double          posx, posy, posz;   // local origin
    double          dirx, diry, dirz;   // direction vector
    double          radius;
    double          cosa, cosb;         // cos(cone angle)
    double          afall;              // cone angle falloff
    double          dist1, dist2;       // axis clipping
    double          dfall;              // distance falloff
};
```

### 2.10 `API_UmatType` — 3D Material

```c
struct API_UmatType {
    API_3D_Head       head;
    API_MaterialType  mater;    // the material definition
    // head.index == 0 for GDL materials; != 0 for global material reference
};
```

### 2.11 `API_Component3D` — Union of all types

```c
union API_Component3D {
    API_3D_Head   header;
    API_BodyType  body;
    API_PgonType  pgon;
    API_PedgType  pedg;
    API_EdgeType  edge;
    API_VertType  vert;
    API_VectType  vect;
    API_LghtType  lght;
    API_UmatType  umat;
};
```

### 2.12 C API Functions

```c
// Get count of components of given type (only BodyID, LghtID, UmatID allowed directly):
GSErrCode ACAPI_ModelAccess_GetNum     (API_3DTypeID typeID, Int32* count);

// Get a single 3D component by type and index:
GSErrCode ACAPI_ModelAccess_GetComponent (API_Component3D* component);

// Decompose a polygon into convex sub-polygons:
GSErrCode ACAPI_ModelAccess_DecomposePgon (Int32 ipgon, Int32*** cpoly);

// Get (u,v) texture coordinates at a surface point:
GSErrCode ACAPI_ModelAccess_GetTextureCoord (API_TexCoordPars* texCoordPars, API_UVCoord* uvCoord);
```

---

## 3. C++ ModelerAPI (`Support/Modules/GSModelDevLib/`)

### 3.1 `ModelerAPI::Model` (`Model.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT Model {
public:
    enum SpecialLightType {
        AmbientLightType = 1,
        CameraLightType  = 2,
        SunLightType     = 3
    };

    Model ();
    Model (const Model& other);
    ~Model ();
    Model& operator= (const Model& other);
    bool   operator== (const Model& other) const;
    bool   operator!= (const Model& other) const;
    bool   operator<  (const Model& other) const;

    ULong  GenerateHashValue () const;
    Box3D  GetBounds (void) const;
    Int32  GetElementCount (void) const;
    Int32  GetColorCount (void) const;
    Int32  GetMaterialCount (void) const;
    Int32  GetTextureCount (void) const;
    Int32  GetFillCount (void) const;
    Int32  GetLightCount (void) const;

    std::optional<Int32> GetElementIndex (const GS::Guid& guid) const;
    void GetElement  (Int32 elementIndex, Element* element) const;
    void GetColor    (const ModelerAPI::AttributeIndex& colorIndex, Color* color) const;
    void GetMaterial (const ModelerAPI::AttributeIndex& materialIndex, Material* material) const;
    void GetTexture  (const ModelerAPI::AttributeIndex& textureIndex, Texture* texture) const;
    void GetTexture  (const char* textureFileName, Texture* texture) const;
    void GetTexture  (const GS::UniString& textureFileName, Texture* texture) const;
    bool IsTextureUsed (const AttributeIndex& textureIndex) const;
    void GetLight    (Int32 lightIndex, Light* light) const;
    void GetLight    (SpecialLightType type, Light* light) const;
    GS::Guid GetGuid (void) const;
};
}
```

### 3.2 `ModelerAPI::Element` (`ModelElement.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT Element {
public:
    enum Type {
        UndefinedElement, WallElement, SlabElement, RoofElement,
        CurtainWallElement, CWFrameElement, CWPanelElement,
        CWJunctionElement, CWAccessoryElement, CWSegmentElement,
        ShellElement, SkylightElement, FreeshapeElement,
        DoorElement, WindowElement, ObjectElement, LightElement,
        ColumnElement, MeshElement, BeamElement, RoomElement,
        StairElement, RiserElement, TreadElement, StairStructureElement,
        RailingElement, ToprailElement, HandrailElement, RailElement,
        RailingPostElement, InnerPostElement, BalusterElement,
        RailingPanelElement, RailingSegmentElement, RailingNodeElement,
        RailPatternElement, InnerTopRailEndElement, InnerHandRailEndElement,
        RailFinishingObjectElement, TopRailConnectionElement,
        HandRailConnectionElement, RailConnectionElement, RailEndElement,
        BalusterSetElement, Opening, Openingframeinfill, Openingpatchinfill,
        ColumnSegmentElement, BeamSegmentElement, OtherElement
    };

    enum class EdgeColorInBaseElemId                        { Included, NotIncluded };
    enum class PolygonAndFaceTextureMappingInBaseElemId     { Included, NotIncluded };
    enum class BodyTextureMappingInBaseElemId               { Included, NotIncluded };
    enum class EliminationInfoInBaseElemId                  { Included, NotIncluded };

    Element ();
    Element (const Element& other);
    ~Element ();
    Element& operator= (const Element& other);
    bool     operator== (const Element& other) const;
    bool     operator!= (const Element& other) const;
    bool     operator<  (const Element& other) const;
    ULong    GenerateHashValue () const;

    bool   IsInvalid (void) const;
    UInt32 GetGenId (void) const;

    Int32  GetTessellatedBodyCount (void) const;
    Int32  GetMeshBodyCount (void) const;
    Int32  GetNurbsBodyCount (void) const;
    Int32  GetPointCloudCount (void) const;
    Int32  GetLightCount (void) const;

    GS::Guid GetElemGuid (void) const;
    Type   GetType (void) const;
    Box3D  GetBounds (const CoordinateSystem = CoordinateSystem::World) const;

    void GetTessellatedBody (Int32 bodyIndex, MeshBody* body) const;
    void GetMeshBody        (Int32 bodyIndex, MeshBody* body) const;
    void GetNurbsBody       (Int32 bodyIndex, NurbsBody* body) const;
    void GetPointCloud      (Int32 pointCloudIndex, PointCloud* pointCloud) const;
    void GetLight           (Int32 lightIndex, Light* light) const;

    Transformation GetElemLocalToWorldTransformation () const;

    void GetBaseElemId (BaseElemId* baseElemId,
                        GS::ProcessControl& processControl,
                        EdgeColorInBaseElemId,
                        PolygonAndFaceTextureMappingInBaseElemId,
                        BodyTextureMappingInBaseElemId,
                        EliminationInfoInBaseElemId) const;

    void GetEliminationInfo (GS::ProcessControl& processControl,
                             EliminationInfo* eliminationInfo) const;
};
}
```

### 3.3 `ModelerAPI::MeshBody` (`ModelMeshBody.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT MeshBody {
public:
    MeshBody ();
    MeshBody (const MeshBody& other);
    ~MeshBody ();
    MeshBody& operator= (const MeshBody& other);

    // Body classification
    bool IsWireBody         (void) const;
    bool IsSurfaceBody      (void) const;
    bool IsSolidBody        (void) const;
    bool IsClosed           (void) const;
    bool IsVisibleIfContour (void) const;
    bool HasSharpEdge       (void) const;
    bool AlwaysCastsShadow  (void) const;
    bool NeverCastsShadow   (void) const;
    bool DoesNotReceiveShadow (void) const;

    // Bounds
    Box3D GetBounds (const CoordinateSystem = CoordinateSystem::World) const;

    // Counts
    Int32 GetVertexCount       (void) const;
    Int32 GetEdgeCount         (void) const;
    Int32 GetPolygonCount      (void) const;
    Int32 GetPolygonVectorCount(void) const;

    // Default color/material/texture at body level
    bool HasColor () const;
    void GetColor         (Color* color) const;
    void GetColorIndex    (AttributeIndex& iCol) const;
    void GetMaterial      (Material* material) const;
    void GetMaterialIndex (AttributeIndex& iMat) const;
    void GetTexture       (Texture* texture) const;
    void GetTextureIndex  (AttributeIndex& iTex) const;

    // Geometry iteration
    void GetVertex  (Int32 vertexIndex, Vertex* vertex,
                     const CoordinateSystem = CoordinateSystem::World) const;
    bool GetVertexHardFlag (Int32 vertexIndex) const;
    void GetEdge    (Int32 edgeIndex, Edge* edge) const;
    void GetPolygon (Int32 polygonIndex, Polygon* polygon) const;
    void GetVector  (Int32 bodyVectorIndex, Vector* vector,
                     const CoordinateSystem = CoordinateSystem::World) const;

    // Texture coordinate system
    const TextureCoordinateSystem* GetTextureCoordinateSystem (void) const;
};
}
```

### 3.4 `ModelerAPI::Polygon` (`Polygon.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT Polygon {
public:
    Polygon ();
    Polygon (const Polygon& other);
    ~Polygon ();
    Polygon& operator= (const Polygon& other);

    bool   IsInvisible        (void) const;
    bool   IsVisibleIfContour (void) const;
    bool   IsComplex          (void) const;
    bool   IsGravity          (void) const;
    Int32  GetEdgeCount       (void) const;
    Int32  GetNormalVectorIndex (void) const;

    // Per-polygon material/texture
    void   GetMaterial      (Material* material) const;
    void   GetMaterialIndex (AttributeIndex& iMat) const;
    void   GetMaterialTexture     (Texture* texture) const;
    void   GetPolygonTexture      (Texture* texture) const;
    void   GetMaterialTextureIndex (AttributeIndex& iText) const;
    void   GetPolygonTextureIndex  (AttributeIndex& iText) const;
    bool   HasMaterialTexture () const;
    bool   HasPolygonTexture  () const;

    // Vertex/edge traversal
    Int32  GetEdgeIndex   (Int32 edgeIndex) const;
    Int32  GetVertexIndex (Int32 vertexIndex) const;

    // Convex decomposition
    Int32  GetConvexPolygonCount (void) const;
    void   GetConvexPolygon (Int32 polygonIndex, ConvexPolygon* polygon) const;

    // Normal vector per vertex (for smooth/curved surfaces)
    Vector GetNormalVectorByVertex (Int32 polygonVertexIndex,
                const CoordinateSystem = CoordinateSystem::World) const;

    // Texture coordinate at a world-space point on this polygon
    void   GetTextureCoordinate (const Vertex* positionInWorldCS,
                                 TextureCoordinate* textureCoordinate) const;

    Int32  GetPolygonId () const;
};
}
```

### 3.5 `ModelerAPI::ConvexPolygon` (`ConvexPolygon.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT ConvexPolygon {
public:
    ConvexPolygon ();
    ConvexPolygon (const ConvexPolygon& other);
    ~ConvexPolygon ();
    ConvexPolygon& operator= (const ConvexPolygon& other);

    Int32  GetVertexCount (void) const;
    Int32  GetVertexIndex (Int32 vertexIndex) const;
    Vector GetNormalVectorByVertex (Int32 vertexIndex,
                const CoordinateSystem = CoordinateSystem::World) const;
};
}
```

### 3.6 `ModelerAPI::Edge` (`ModelEdge.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT Edge {
public:
    Edge ();
    Edge (const Edge& other);
    ~Edge ();
    Edge& operator= (const Edge& other);

    bool IsInvisible        (void) const;
    bool IsVisibleIfContour (void) const;
    bool HasColor           () const;
    void GetColor      (Color* color) const;
    void GetColorIndex (AttributeIndex& iCol) const;

    Int32 GetVertexIndex1  (void) const;
    Int32 GetVertexIndex2  (void) const;
    Int32 GetPolygonIndex1 (void) const;   // -1 if none
    Int32 GetPolygonIndex2 (void) const;   // -1 if none
};
}
```

### 3.7 `ModelerAPI::Vertex` (`Vertex.hpp`)

```cpp
namespace ModelerAPI {
class Vertex {
public:
    double x, y, z;
    Vertex () : x(0.0), y(0.0), z(0.0) {}
    Vertex (double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
};
}
```

### 3.8 `ModelerAPI::Vector` (normal) (`ModelVector.hpp`)

```cpp
namespace ModelerAPI {
class Vector {
public:
    double x, y, z;
    Vector () : x(0.0), y(0.0), z(0.0) {}
    Vector (double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vector (const Vector& v) : x(v.x), y(v.y), z(v.z) {}

    double  GetLengthSqr () const;
    Vector  operator- (const Vector& v) const;
    Vector& operator= (const Vector& v);
};
}
```

### 3.9 `ModelerAPI::Color` (`ModelColor.hpp`)

```cpp
namespace ModelerAPI {
class Color {
public:
    double red, green, blue;    // 0.0 .. 1.0 range
    Color () : red(0.0), green(0.0), blue(0.0) {}
    Color (double r, double g, double b) : red(r), green(g), blue(b) {}
    bool operator== (const Color& other) const;
    bool operator!= (const Color& other) const;
    bool operator<  (const Color& other) const;
};
}
```

### 3.10 `ModelerAPI::Light` (`Light.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT Light {
public:
    enum Type {
        UndefinedLight = 0,
        DirectionLight = 9200,
        SpotLight      = 9300,
        PointLight     = 9400,
        SunLight       = 9500,
        EyeLight       = 9600,
        AmbientLight   = 9700,
        CameraLight    = 9800
    };

    Light ();
    Light (const Light& other);
    ~Light ();
    Light& operator= (const Light& other);

    Type   GetType    (void) const;
    bool   CastsShadow(void) const;
    Color  GetColor   (void) const;
    Vertex GetPosition(const CoordinateSystem = CoordinateSystem::World) const;
    Vector GetDirection(const CoordinateSystem = CoordinateSystem::World) const;
    Vector GetUpVector(const CoordinateSystem = CoordinateSystem::World) const;
    double GetRadius       (void) const;
    double GetAngleFalloff (void) const;
    double GetFalloffAngle1(void) const;
    double GetFalloffAngle2(void) const;
    double GetDistanceFalloff(void) const;
    double GetMinDistance  (void) const;
    double GetMaxDistance  (void) const;
    void   GetExtraParameters (ParameterList* parameters) const;
};
}
```

### 3.11 `ModelerAPI::PointCloud` (`ModelPointCloud.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT PointCloud {
public:
    PointCloud ();
    PointCloud (const PointCloud& other);
    ~PointCloud ();
    PointCloud& operator= (const PointCloud& other);

    IPointCloudClip* GetClip () const;
    PC::Matrix       GetDataToTargetCoordSysTransformation (
                         const CoordinateSystem = CoordinateSystem::World) const;
    Box3D            GetBounds (
                         const CoordinateSystem = CoordinateSystem::World) const;
};
}
```

### 3.12 Coordinate System (`CoordinateSystem.hpp`)

```cpp
namespace ModelerAPI {
    enum class CoordinateSystem : short {
        ElemLocal,
        World
    };
}
```

### 3.13 Transformation (`Transformation.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT Transformation {
public:
    short  status;
    double matrix[3][4];   // affine transform + translation
    Transformation ();
    void SetToIdentity ();
    void FromTRANMAT (const TRANMAT* tran);
    void ToTRANMAT   (TRANMAT* tran) const;
};
}
```

---

## 4. Material & Texture (`Support/Modules/GSModelDevLib/`)

### 4.1 `ModelerAPI::Material` (`ModelMaterial.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT Material {
public:
    enum Type {
        General  = 0,
        Simple   = 1,
        Matte    = 2,
        Metal    = 3,
        Plastic  = 4,
        Glass    = 5,
        Glowing  = 6,
        Constant = 7
    };

    Material ();
    Material (const Material& other);
    Material (Material&& other);
    ~Material ();
    Material& operator= (const Material& other);
    bool operator== (const Material& other) const;
    bool operator!= (const Material& other) const;
    bool operator<  (const Material& other) const;

    ULong          GenerateHashValue () const;
    Type           GetType () const;
    GS::UniString  GetName () const;
    Color          GetSurfaceColor () const;
    double         GetAmbientReflection () const;
    double         GetDiffuseReflection () const;
    double         GetSpecularReflection () const;
    Color          GetSpecularColor () const;
    double         GetTransparency () const;
    double         GetTransparencyAttenuation () const;
    double         GetShining () const;
    Color          GetEmissionColor () const;
    double         GetEmissionAttenuation () const;
    void           GetTextureIndex (AttributeIndex& iText) const;
    bool           HasTexture () const;
    void           GetTextureName (char* str) const;
    GS::UniString  GetTextureName () const;
    double         GetTextureRotationAngle () const;
    void           GetFillIndex (AttributeIndex& iFill) const;
    void           GetFillColorIndex (AttributeIndex& iFillColor) const;
    void           GetTexture (Texture* texture) const;
    Int32          GetExternalReference (void) const;
    void           GetExtraParameters (ParameterList* parameters) const;

    // Setters (for construction)
    void SetType (Type type);
    void SetName (char* str);
    void SetSurfaceColor (Color color);
    void SetAmbientReflection (double);
    void SetDiffuseReflection (double);
    void SetSpecularReflection (double);
    void SetSpecularColor (Color);
    void SetTransparency (double);
    void SetTransparencyAttenuation (double);
    void SetShining (double);
    void SetEmissionColor (Color);
    void SetEmissionAttenuation (double);
    void SetTextureName (char* str);
    void SetTextureWidth (double);
    void SetTextureHeight (double);
    void SetTextureRotationAngle (double);
    void SetTextureStatus (short);
    void SetFillIndex (const AttributeIndex&);
    void SetFillColorIndex (short);
};
}
```

### 4.2 `ModelerAPI::Texture` (`Texture.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT Texture {
public:
    enum PixelType {
        ARGBPixelType           = 0x0001,
        Grayscale8PixelType     = 0x0002,
        BlackAndWhitePixelType  = 0x0003
    };

    enum Status {
        UseAlpha          = 0x0001,
        TransparentPattern = 0x0008,
        BumpMapPattern    = 0x0010,
        DiffusePattern    = 0x0020,
        SpecularPattern   = 0x0040,
        AmbientPattern    = 0x0080,
        SurfacePattern    = 0x0100,
        RandomShift       = 0x0200,
        MirrorX           = 0x0400,
        MirrorY           = 0x0800,
        TextureLinkMat    = 0x1000
    };

    enum TextureOrigin { File, Model, SightBackground };

    class ARGBPixel {      // 32-bit
    public:
        unsigned char alpha, red, green, blue;
    };

    class Grayscale8Pixel { // 8-bit
    public:
        unsigned char value;
    };

    enum BlackAndWhitePixel { Black = 0x00, White = 0x01 };

    Texture ();
    Texture (const Texture& other);
    ~Texture ();
    Texture& operator= (const Texture& other);
    bool operator== (const Texture& other) const;
    bool operator!= (const Texture& other) const;
    bool operator<  (const Texture& other) const;

    ULong          GenerateHashValue () const;
    bool           HasAlphaChannel () const;
    bool           IsAvailable () const;
    bool           IsTransparentPattern () const;
    bool           IsBumpMapPattern () const;
    bool           IsDiffusePattern () const;
    bool           IsSpecularPattern () const;
    bool           IsAmbientPattern () const;
    bool           IsSurfacePattern () const;
    bool           IsShiftedRandomly () const;
    bool           IsMirroredInX () const;
    bool           IsMirroredInY () const;
    Int32          GetGDLStatus () const;
    double         GetXSize () const;
    double         GetYSize () const;
    Int32          GetPixelMapXSize () const;
    Int32          GetPixelMapYSize () const;
    Int32          GetPixelMapSize () const;
    PixelType      GetPixelType () const;
    GS::UniString  GetName () const;

    // Pixel access by (x,y) index:
    void GetPixelColor (Int32 x, Int32 y, Color* color, double* alpha = nullptr) const;
    void GetPixel      (Int32 x, Int32 y, ARGBPixel* pixel) const;
    void GetPixel      (Int32 x, Int32 y, Grayscale8Pixel* pixel) const;
    void GetPixel      (Int32 x, Int32 y, BlackAndWhitePixel* pixel) const;

    // Pixel access by UV coordinate:
    void GetPixelColor (const TextureCoordinate* texCoord, Color* color, double* alpha = nullptr) const;
    void GetPixel      (const TextureCoordinate* texCoord, ARGBPixel* pixel) const;
    void GetPixel      (const TextureCoordinate* texCoord, Grayscale8Pixel* pixel) const;
    void GetPixel      (const TextureCoordinate* texCoord, BlackAndWhitePixel* pixel) const;

    // Bulk pixel map retrieval:
    void GetPixelMap (ARGBPixel* pixelMap) const;
    void GetPixelMap (Grayscale8Pixel* pixelMap) const;
    void GetPixelMap (BlackAndWhitePixel8* pixelMap) const;

    Int32          GetPixelMapBufferSize () const;
    void           GetPixelMapCheckSum (char* checkSum, Int32 strLen) const;
    GS::UniString  GetPixelMapCheckSum () const;
    GS::UniString  GetFingerprint () const;
    const GDL::IFileRef* GetExternalReference () const;
};
}
```

### 4.3 `ModelerAPI::TextureCoordinate` & `TextureCoordinateSystem` (`TextureCoordinate.hpp`)

```cpp
namespace ModelerAPI {

class GSMODELER_DLL_EXPORT TextureCoordinate {
public:
    double u, v;
    void ApplyRotationAndScale (double rotAngle, double xScale, double yScale);
    void ApplyMaterialParameters (double rotAngle, double xSize, double ySize);
};

struct TextureCoordinateSystem {
    enum TransformationMode {
        InvalidMode    = 0,
        BoxMode        = 1,
        CylindricMode  = 2,
        SphericMode    = 3,
        NurbsParamMode = 7
    };

    TransformationMode transformationMode;
    Vertex origo;
    Vector xAxis, yAxis, zAxis;

    TextureCoordinateSystem ();
    TextureCoordinateSystem (TransformationMode, Vertex origo,
                             Vector xAxis, Vector yAxis, Vector zAxis);
};

}
```

### 4.4 C API — Texture Coordinate (`APIdefs_Base.h`, `APIdefs_Goodies.h`)

```c
struct API_UVCoord {
    double u, v;
};

struct API_TexCoordPars {
    UInt32      elemIdx;       // element index
    UInt32      bodyIdx;       // 3D body index
    UInt32      pgonIndex;     // polygon index within this body
    UInt32      filler_1;
    API_Coord3D surfacePoint;  // point in local coordinates
};
```

---

## 5. NURBS Body API (`Support/Modules/GSModelDevLib/`)

### 5.1 `ModelerAPI::NurbsBody` (`ModelNurbsBody.hpp`)

```cpp
namespace ModelerAPI {
class GSMODELER_DLL_EXPORT NurbsBody {
public:
    NurbsBody ();
    NurbsBody (const NurbsBody& other);
    ~NurbsBody ();
    NurbsBody& operator= (const NurbsBody& other);

    Box3D                GetBounds (const CoordinateSystem = CoordinateSystem::World) const;

    AttributeIndex       GetEdgePenIdx  () const;
    Color                GetEdgePen     () const;
    AttributeIndex       GetMaterialIdx () const;
    Material             GetMaterial    () const;
    bool                 AlwaysCastsShadow    () const;
    bool                 NeverCastsShadow     () const;
    bool                 DoesNotReceiveShadow () const;
    Interval             GetSmoothness () const;
    TextureCoordinateSystem GetTextureCoordSys () const;

    // Topology counts
    UInt32 GetVertexCount   () const;
    UInt32 GetEdgeCount     () const;
    UInt32 GetTrimCount     () const;
    UInt32 GetLoopCount     () const;
    UInt32 GetFaceCount     () const;
    UInt32 GetShellCount    () const;
    UInt32 GetLumpCount     () const;
    UInt32 GetCurve2DCount  () const;
    UInt32 GetCurve3DCount  () const;
    UInt32 GetSurfaceCount  () const;

    // Topology access
    NurbsVertex          GetVertex  (UInt32 index, const CoordinateSystem = CoordinateSystem::World) const;
    NurbsEdge            GetEdge    (UInt32 index, const CoordinateSystem = CoordinateSystem::World) const;
    NurbsTrim            GetTrim    (UInt32 index) const;
    NurbsLoop            GetLoop    (UInt32 index) const;
    NurbsFace            GetFace    (UInt32 index, const CoordinateSystem = CoordinateSystem::World) const;
    NurbsShell           GetShell   (UInt32 index) const;
    NurbsLump            GetLump    (UInt32 index) const;

    // Geometry access
    Geometry::NurbsCurve2D   GetCurve2D (UInt32 index) const;
    Geometry::NurbsCurve3D   GetCurve3D (UInt32 index, const CoordinateSystem = CoordinateSystem::World) const;
    Geometry::NurbsSurface   GetSurface (UInt32 index, const CoordinateSystem = CoordinateSystem::World) const;

    // Attributes
    NurbsVertexAttributes GetVertexAttributes (UIndex index) const;
    NurbsEdgeAttributes   GetEdgeAttributes   (UIndex index) const;
    NurbsFaceAttributes   GetFaceAttributes   (UIndex index) const;
};
}
```

### 5.2 NURBS Sub-elements

**`NurbsVertex`** (`ModelNurbsVertex.hpp`):
```cpp
class NurbsVertex : public Vertex, public NurbsElementWithTolerance {
public:
    NurbsVertex (double x, double y, double z, double tolerance);
    // Inherits: double x, y, z; double GetTolerance() const;
};
```

**`NurbsEdge`** (`ModelNurbsEdge.hpp`):
```cpp
class NurbsEdge : public NurbsElementWithTolerance {
public:
    bool     IsLoopEdge        () const;  // beginVertex == endVertex (>= 0)
    bool     IsRingEdge        () const;  // beginVertex < 0 && endVertex < 0
    bool     IsRegularEdge     () const;  // neither loop nor ring
    bool     IsWire            () const;  // 0 trims
    bool     IsSurfaceBoundary () const;  // 1 trim
    bool     Is2Manifold       () const;  // 2 trims
    bool     IsNon2Manifold    () const;  // >2 trims
    Int32    GetBeginVertexIndex  () const;
    Int32    GetEndVertexIndex    () const;
    UInt32   GetTrimIndexCount    () const;
    UInt32   GetTrimIndex         (UInt32 index) const;  // 1-based
    UInt32   GetCurve3DIndex      () const;
    Interval GetCurveSubdomain    () const;
};
```

**`NurbsTrim`** (`ModelNurbsTrim.hpp`):
```cpp
class NurbsTrim : public NurbsElementWithTolerance {
public:
    Int32    GetEdgeIndex        () const;  // negative for singular trim
    Int32    GetVertexIndex      () const;  // nonnegative for singular trim
    UInt32   GetLoopIndex        () const;
    Int32    GetTrimcurve2DIndex  () const;
    Interval GetCurveSubdomain   () const;
    bool     IsSingular          () const;  // edgeIndex < 0
};
```

**`NurbsLoop`** (`ModelNurbsLoop.hpp`):
```cpp
class NurbsLoop {
public:
    bool     IsTrimReversed (UInt32 index) const;
    UInt32   GetTrimIndexCount () const;
    DirectedTrimIndex GetTrimIndex (UInt32 index) const;  // 1-based
    UInt32   GetFaceIndex () const;
};
// DirectedTrimIndex = { UInt32 trim; bool reversed; }
```

**`NurbsFace`** (`ModelNurbsFace.hpp`):
```cpp
class NurbsFace : public NurbsElementWithTolerance {
public:
    UInt32 GetLoopIndexCount () const;
    UInt32 GetLoopIndex      (UInt32 index) const;  // 1-based, first is outer loop
    Int32  GetShellIndex     () const;  // negative for lamina face
    UInt32 GetSurfaceIndex   () const;
};
```

**`NurbsShell`** (`ModelNurbsShell.hpp`):
```cpp
class NurbsShell {
public:
    UInt32              GetFaceIndexCount () const;
    DirectedFaceIndex   GetFaceIndex      (UInt32 index) const;  // 1-based
    UInt32              GetLumpIndex      () const;
};
// DirectedFaceIndex = { UInt32 face; bool reversed; }
```

**`NurbsLump`** (`ModelNurbsLump.hpp`):
```cpp
class NurbsLump {
public:
    UInt32 GetShellIndexCount () const;
    UInt32 GetShellIndex      (UInt32 index) const;  // 1-based, first is outer shell
};
```

### 5.3 `NurbsAttributes` (`NurbsAttributes.hpp`)

```cpp
struct NurbsVertexAttributes {
    enum class Hardness : GS::UChar { Soft, Hard };
    Hardness hardness;
};

struct NurbsEdgeAttributes {
    using Color = short;
    enum class Visibility : GS::UChar  { Visible, Invisible, VisibleIfContour };
    enum class Smoothness : GS::UChar { Sharp, Smooth };
    Visibility  visibility : 2;
    Smoothness  smoothness : 1;
    Color       color;
};

struct NurbsFaceAttributes {
    using Pen = short;
    GSAttributeIndex        material;
    Pen                     segmentationPen;
    TextureCoordinateSystem textureCoordSys;  // NurbsParamMode for parametric UV
};
```

---

## 6. C API — Texture Coordinate access (`ACAPI_ModelAccess_GetTextureCoord`)

From `Support/Inc/ACAPI_Goodies.h` and `APIdefs_Base.h`, `APIdefs_Goodies.h`:

```c
struct API_TexCoordPars {
    UInt32      elemIdx;       // from API_BodyType.head.elemIndex (minus 1)
    UInt32      bodyIdx;       // from API_BodyType.head.bodyIndex (minus 1)
    UInt32      pgonIndex;     // polygon index within the body
    UInt32      filler_1;
    API_Coord3D surfacePoint;  // 3D point on the polygon (local coords)
};

struct API_UVCoord {
    double u;
    double v;
};

GSErrCode ACAPI_ModelAccess_GetTextureCoord (
    API_TexCoordPars* texCoordPars,
    API_UVCoord*      uvCoord
);
```

---

## 7. Low-Level BREP Data Structures (`Support/Modules/Brep/`)

### 7.1 `Brep::MeshBrep` (`MeshBrep.hpp`)

```cpp
namespace Brep {

class BREP_DLL_EXPORT MeshBrep {
public:
    typedef Coord3D    Vertex;         // double x,y,z
    typedef Int32      PolyEdge;
    typedef Vector3D   PolyNormal;     // double x,y,z

    struct Edge {
        Int32 vert1 = 0, vert2 = 0;
        Int32 pgon1 = 0, pgon2 = 0;    // -1 if none (InvalidPgonIdx)

        Int32  GetAnotherVertex  (Int32 currentVertex) const;
        Int32  GetAnotherPolygon (Int32 currentPolygon) const;
        UInt32 GetPolygonCount   () const;
    };

    struct Polygon {
        Int32 ivect;    // signed normal index (negative = opposite direction)
        Int32 fpedg;    // first polyEdge index
        Int32 lpedg;    // last polyEdge index
        UInt32 GetEdgeCount () const;
    };

    MeshBrep ();

    // Counts
    ULong GetVertexCount     () const;
    ULong GetEdgeCount       () const;
    ULong GetPolyEdgeCount   () const;
    ULong GetPolyNormalCount () const;
    ULong GetPolygonCount    () const;

    // Read-only iteration
    Iterator Polygons:     BeginPolygons() / EndPolygons()
    Iterator PolyNormals:  BeginPolyNormals() / EndPolyNormals()
    Iterator PolyEdges:    BeginPolyEdges() / EndPolyEdges()
    Iterator Edges:        BeginEdges() / EndEdges()
    Iterator Vertices:     BeginVertices() / EndVertices()

    // Direct index access (0-based)
    const Vertex&     GetConstVertex    (ULong index) const;
    const Edge&       GetConstEdge      (ULong index) const;
    const Polygon&    GetConstPolygon   (ULong index) const;
    PolyEdge          GetConstPolyEdge  (ULong index) const;  // signed: edge index; 0 = hole marker
    const PolyNormal& GetConstPolyNormal(ULong index) const;
    PolyNormal        GetDirectedPolyNormalVector (ULong polygonIndex) const;
    Int32             GetBeginVertexOfPolyEdge (PolyEdge polyEdge) const;

    // Bounds
    const F_Box3D& GetLocalBounds() const;
    void           CalculateTransformedBounds (const TRANMAT& trafo, F_Box3D& outBounds) const;
};
}
```

**`MeshBrepFB`** — Fixed-buffer variant with preallocated capacity:
```cpp
class BREP_DLL_EXPORT MeshBrepFB : public MeshBrep {
    static constexpr UInt32 VertexAllocCount    = 127;
    static constexpr UInt32 EdgeAllocCount      = 127;
    static constexpr UInt32 PolyEdgeAllocCount  = 2 * EdgeAllocCount;  // 254
    static constexpr UInt32 PolyNormalAllocCount = 63;
    static constexpr UInt32 PolygonAllocCount    = 63;
};
```

### 7.2 `Brep::MeshBrepAccessors` (`MeshBrepAccessors.hpp`)

Free function templates for traversing `MeshBrep`. Work with both `MeshBrep` and `MeshBrepFB`.

```cpp
namespace Brep {

// Get polygon normal (signed index → TRANMAT-transformed vector):
template<typename MeshBrepT>
MeshBrep::PolyNormal GetMeshPolygonNormalFromSignedIndex (
    const MeshBrepT& meshBrep, Int32 ivect, const TRANMAT& trafo);

// Get polygon normal by polygon index:
template<typename MeshBrepT>
MeshBrep::PolyNormal GetMeshPolygonNormalVector (
    const MeshBrepT& meshBrep, UIndex polygonIndex, const TRANMAT& trafo = IdentityTranMat);

// Get a polyEdge by polygon index + edge index within polygon:
template<typename MeshBrepT>
MeshBrep::PolyEdge GetMeshPolygonPolyEdge (
    const MeshBrepT& meshBrep, UIndex polygonIndex, UInt32 edgeIdx);

// Get an oriented edge (flipped if polyEdge is negative):
template<typename MeshBrepT>
MeshBrep::Edge GetMeshPolygonOrientedEdge (
    const MeshBrepT& meshBrep, UIndex polygonIndex, UInt32 edgeIdx);

// Get a vertex by polygon + vertex index. holeMarker=true if vertex is a hole boundary:
template<typename MeshBrepT>
MeshBrep::Vertex GetMeshPolygonVertex (
    const MeshBrepT& meshBrep, UIndex polygonIndex, UInt32 vertexIdx,
    bool* holeMarker, const TRANMAT& trafo = IdentityTranMat);

// Count holes in a polygon:
template<typename MeshBrepT>
UInt32 GetMeshPolygonHoleCount (
    const MeshBrepT& meshBrep, UIndex polygonIndex);

// Traverse a polygon's contour (wrap around holes):
template<typename MeshBrepT>
Int32 GetMeshPolygonContourNextIPedg (
    const MeshBrepT& meshBrep, UIndex polygonIndex, Int32 ipedg);
template<typename MeshBrepT>
Int32 GetMeshPolygonContourPrevIPedg (
    const MeshBrepT& meshBrep, UIndex polygonIndex, Int32 ipedg);

// Bounding box of a single polygon:
template<typename MeshBrepT>
Box3D GetMeshPolygonBounds (
    const MeshBrepT& meshBrep, UIndex polygonIndex, const TRANMAT& trafo = IdentityTranMat);

// Plane equation (normal + point) of a polygon:
template<typename MeshBrepT>
PlaneEq GetMeshPolygonPlane (
    const MeshBrepT& meshBrep, UIndex polygonIndex, const TRANMAT& trafo);

// Get vertex index from a polyEdge index:
template <typename MeshBrepT>
Int32 GetMeshVertexIndexFromPolyEdgeIndex (
    const MeshBrepT& meshBrep, Int32 pedgIdx);

// Transform a vertex by TRANMAT:
template <typename MeshBrepT>
MeshBrep::Vertex GetMeshTransformedVertex (
    const MeshBrepT& meshBrep, UIndex vertexIndex, const TRANMAT& trafo);
}
```

### 7.3 `Brep::NurbsBrep` (`NurbsBrep.hpp`)

```cpp
namespace Brep {

class BREP_DLL_EXPORT NurbsBrep {
public:
    static const UInt32 NurbsVertexAllocCount    = 12;
    static const UInt32 NurbsEdgeAllocCount      = 18;
    static const UInt32 NurbsTrimAllocCount      = 36;
    static const UInt32 NurbsLoopAllocCount      = 9;
    static const UInt32 NurbsFaceAllocCount      = 9;
    static const UInt32 NurbsShellAllocCount     = 1;
    static const UInt32 NurbsRegionAllocCount    = 2;

    NurbsBrep ();

    // Counts
    UIndex GetVertexCount  () const;
    UIndex GetEdgeCount    () const;
    UIndex GetTrimCount    () const;
    UIndex GetLoopCount    () const;
    UIndex GetFaceCount    () const;
    UIndex GetShellCount   () const;
    UIndex GetLumpCount    () const;
    UIndex GetCurve2DCount () const;
    UIndex GetCurve3DCount () const;
    UIndex GetSurfaceCount () const;

    // Direct indexed access (0-based) — NO begin/end iterators, index access only:
    const NurbsVertex&            GetConstVertex  (UIndex i) const;
    const NurbsEdge&              GetConstEdge    (UIndex i) const;
    const NurbsTrim&              GetConstTrim    (UIndex i) const;
    const NurbsLoop&              GetConstLoop    (UIndex i) const;
    const NurbsFace&              GetConstFace    (UIndex i) const;
    const NurbsShell&             GetConstShell   (UIndex i) const;
    const NurbsLump&              GetConstLump    (UIndex i) const;
    const Geometry::NurbsCurve2D& GetConstCurve2D (UIndex i) const;
    const Geometry::NurbsCurve3D& GetConstCurve3D (UIndex i) const;
    const Geometry::NurbsSurface& GetConstSurface (UIndex i) const;

    // Faces using an edge:
    class ConstNurbsEdgeOfFaceIterator;
    ConstNurbsEdgeOfFaceIterator BeginEdgeOfFaceIterator (UIndex faceIndex) const;

    // Edges belonging to a face:
    class ConstNurbsFaceOfEdgeIterator;
    ConstNurbsFaceOfEdgeIterator BeginFaceOfEdgeIterator (UIndex edgeIndex) const;

    // Trims of a face:
    class ConstNurbsTrimOfFaceIterator;
    ConstNurbsTrimOfFaceIterator BeginTrimOfFaceIterator (UIndex faceIndex) const;

    // Bounds
    Box3D GetBounds (const TRANMAT& tran) const;
    void  Check () const;
};
}
```

### 7.4 Low-Level NURBS Types (`Support/Modules/Brep/`)

**`Brep::NurbsVertex`** (`NurbsVertex.hpp`):
```cpp
class BREP_DLL_EXPORT NurbsVertex {
public:
    Coord3D coord;    // double x,y,z
    double  tolerance;
};
```

**`Brep::NurbsEdge`** (`NurbsEdge.hpp`):
```cpp
class BREP_DLL_EXPORT NurbsEdge {
    // Connects 0,1,2 faces; may be wire, boundary, or 2-manifold edge.
    // Holds: beginVertexIndex, endVertexIndex, trimIndices, curve3DIndex, curveSubDomain, tolerance
};
```

**`Brep::NurbsTrim`** (`NurbsTrim.hpp`):
```cpp
class BREP_DLL_EXPORT NurbsTrim {
    // Connects face to edge (or vertex for singular trim).
    // Holds: edgeIndex, vertexIndex, loopIndex, trimCurve2DIndex, curveSubDomain, tolerance
};
```

**`Brep::NurbsLoop`** (`NurbsLoop.hpp`):
```cpp
class BREP_DLL_EXPORT NurbsLoop {
    // Circular sequence of directed trims.
    // Holds: trimIndices, faceIndex
};
```

**`Brep::NurbsFace`** (`NurbsFace.hpp`):
```cpp
class BREP_DLL_EXPORT NurbsFace {
    // Connected part of a surface bounded by loops.
    // Holds: loopIndices, shellIndex, surfaceIndex, tolerance
};
```

**`Brep::NurbsShell`** (`NurbsShell.hpp`):
```cpp
class BREP_DLL_EXPORT NurbsShell {
    // Closed set of faces connected at edges.
    // Holds: faceIndices, lumpIndex, closedFlag
};
```

**`Brep::NurbsLump`** (`NurbsLump.hpp`):
```cpp
class BREP_DLL_EXPORT NurbsLump {
    // Finite part of 3D space bounded by shells.
    // Holds: shellIndices
};
```

---

## 8. Auxiliary C API Types

### 8.1 `API_ElemInfo3D` — element-to-body mapping (`APIdefs_Elements.h`)

```c
struct API_ElemInfo3D {
    UInt32 fElemIdx;   // incoming element index
    UInt32 fBodyIdx;   // always zero
    UInt32 lElemIdx;   // incoming element index
    UInt32 lBodyIdx;   // last body index for this element (flat list)
    Int32  fbody;      // first body index that belongs to this element
    Int32  lbody;      // last body index that belongs to this element
    // ... more fields
};
```

### 8.2 `API_ElemInfo3D` usage pattern
1. Call `ACAPI_ModelAccess_Get3DInfo(&elemHead, &info3D)` to discover body range `[fbody .. lbody]`.
2. Use `ACAPI_ModelAccess_GetComponent()` with `API_BodyID` for each body in that range.
3. From each `API_BodyType`, iterate subcomponents using indices 1 to `nPgon`, `nPedg`, etc.

---

## 9. Additional Geometry Functions

### 9.1 Cutting / Section

```c
GSErrCode ACAPI_CuttingPlane_GetCutPolygonInfo (
    Int32                                bodyIndex,
    const API_Plane3D&                   cutPlane,
    GS::Array<Geometry::MultiPolygon2D>* resPolygons = nullptr,
    double*                              area        = nullptr
);

GSErrCode ACAPI_CuttingPlane_GetCutPolygonInfo_New (
    Int32                                elemIdx,
    Int32                                bodyIdx,
    const API_Plane3D&                   cutPlane,
    GS::Array<Geometry::MultiPolygon2D>* resPolygons = nullptr,
    double*                              area        = nullptr
);
```

### 9.2 Connection Table

```c
GSErrCode ACAPI_ModelAccess_GetConnectionTable (
    const GS::HashSet<API_Guid>&  elementList,
    API_ElementConnectionTable*   connectionTable
);
```

### 9.3 Building Material

```c
GSErrCode ACAPI_ModelAccess_GetBuildingMaterial (
    const UInt32 elemIdx,
    const UInt32 bodyIdx,
    API_AttributeIndex* materialIdx
);
// Requires prior call to ACAPI_ModelAccess_GenerateModelWithSeparateComponents()
```

### 9.4 GDL Script → Model

```c
GSErrCode ACAPI_LibraryManagement_InterpretGDLScript3D (
    const GS::UniString* script,
    API_AddParType**     addPars,           // can be NULL
    void*                modelerAPIModel,   // ModelerAPI::Model* (optional)
    void*                modelerModel       // Modeler::ConstModel3DPtr* (optional)
);
// Use either modelerAPIModel or modelerModel, not both.
```

---

## 10. Geometry Primitives (`Support/Modules/Geometry/`)

Key low-level geometry types used by both C and C++ APIs:

| Type | Header |
|------|--------|
| `Coord3D` / `Point3D` | `Point3DData.h` |
| `Vector3D` | `Vector3D.hpp` |
| `Box3D` / `F_Box3D` | `Box3DData.h` |
| `Plane3D` | `Plane3D.hpp` |
| `PlaneEq` | `Plane3DData.h` (normal + origin) |
| `TRANMAT` | `TM.h` (3x4 affine transformation) |
| `Polygon2D` | `Polygon2D.hpp` |
| `Polygon3D` | `Polygon3D.hpp` |
| `MultiPolygon2D` | (polygon-with-holes) |
| `NurbsCurve2D` | `NurbsCurve2D.hpp` |
| `NurbsCurve3D` | `NurbsCurve3D.hpp` |
| `NurbsSurface` | `NurbsSurface.hpp` |

---

## 11. Summary: What Details Are Available

| Detail | C API | C++ ModelerAPI |
|--------|-------|----------------|
| **Vertices** (x,y,z) | `API_VertType` — `double` in model space | `Vertex` — `double`; coordinate system selector |
| **Edges** (topology) | `API_EdgeType` — vert1, vert2, pgon1, pgon2 | `Edge` — vertex/polygon indices, visibility |
| **Polygons** | `API_PgonType` — fpedg/lpedg edge ranges, ivect normal | `Polygon` — edge/vertex indices, convex decomposition, per-vertex normals |
| **Normals** | `API_VectType` — per-polygon, signed index (negate if negative) | `Vector` — per-polygon via `GetNormalVectorIndex`, per-vertex via `GetNormalVectorByVertex()` |
| **Materials** | `API_UmatType` — surface properties | `Material` — type, surface color, reflectivity, transparency, emission, fill index |
| **Colors** | `color` (short) on body/edge | `Color` — `double` RGB (0..1) on body/edge/polygon |
| **Textures** | `ACAPI_ModelAccess_GetTextureCoord` — (u,v) at a point | `TextureCoordinate` — per-polygon UV; `Texture` pixel map; `TextureCoordinateSystem` (Box/Cylindrical/Spherical/NurbsParam) |
| **Body bounds** | `xmin..zmax` (`float`) + `API_Tranmat` | `GetBounds(CoordinateSystem)` — `Box3D` |
| **NURBS topology** | Not available | Full topology tree: Vertex → Edge → Trim → Loop → Face → Shell → Lump |
| **NURBS geometry** | Not available | `NurbsCurve2D`, `NurbsCurve3D`, `NurbsSurface` |
| **Lights** | `API_LghtType` — type, position, direction, cone, falloff | `Light` — same plus up-vector and extra parameters |
| **Point clouds** | Not available | `PointCloud` — bounds, coordinate transform, clip |
| **Morph construction** | `ACAPI_Body_Create` / `_AddVertex` / `_AddEdge` / `_AddPolygon` / `_Finish` → `ACAPI_Element_Create` | — |
| **Morph booleans** | `ACAPI_Element_SolidOperation_Create` (subtract/intersect/union) | — |
| **Cutting/sections** | `ACAPI_CuttingPlane_GetCutPolygonInfo` | — |

---

## 12. Morph Body Construction API (`Support/Inc/APIdefs_Elements.h`, `ACAPinc.h`)

Create or modify Morph elements (triangulated/mesh geometry, `API_MorphID = 45`) by
building `Modeler::MeshBody` data through the Body Construction API.

**Sources:** `Support/Inc/ACAPinc.h` (Body subgroup, lines 5786–5928),
`Support/Inc/APIdefs_Elements.h` (API_MorphType, API_ElementMemo.morphBody),
`Examples/Element_Test/Src/Element_Basics.cpp` (full create-from-scratch).

### 12.1 Key Types

```c
// Morph body classification
typedef enum {
    APIMorphBodyType_SurfaceBody,
    APIMorphBodyType_SolidBody
} API_MorphBodyTypeID;

// Morph edge display
typedef enum {
    APIMorphEdgeType_SoftHiddenEdge,
    APIMorphEdgeType_HardHiddenEdge,
    APIMorphEdgeType_HardVisibleEdge
} API_MorphEdgeTypeID;

// Texture projection on morph faces
typedef enum {
    APITextureProjectionType_Invalid,
    APITextureProjectionType_Planar,
    APITextureProjectionType_Default,
    APITextureProjectionType_Cylindric,
    APITextureProjectionType_Spheric,
    APITextureProjectionType_Box
} API_TextureProjectionTypeID;

// ── stored in API_ElementMemo ──
// Modeler::MeshBody*  morphBody;               // the actual mesh geometry
// API_OverriddenAttribute* morphMaterialMapTable; // per-face material overrides
```

### 12.2 Body Construction API

```c
// Start building a body. Pass an existing MeshBody* to import geometry,
// or nullptr to build from scratch.
GSErrCode ACAPI_Body_Create (
    const Modeler::MeshBody*        body,                // optional existing geometry
    const API_OverriddenAttribute*  bodyMaterialMapTable,// optional material table
    void**                          bodyData             // out: opaque builder handle
);

// Finalize: produces MeshBody* + material table suitable for API_ElementMemo.
GSErrCode ACAPI_Body_Finish (
    void*                       bodyData,
    Modeler::MeshBody**         body,                  // out
    API_OverriddenAttribute**   bodyMaterialMapTable   // out
);

// Free builder resources after Finish.
GSErrCode ACAPI_Body_Dispose (void** bodyData);

// Add a vertex. Returns its index (1-based) in *index.
GSErrCode ACAPI_Body_AddVertex (
    void*               bodyData,
    const API_Coord3D&  coord,
    UInt32&             index
);

// Add an edge between two vertex indices.
GSErrCode ACAPI_Body_AddEdge (
    void*   bodyData,
    UInt32  vertex1,
    UInt32  vertex2,
    Int32&  index
);

// Add a polygon normal vector.
GSErrCode ACAPI_Body_AddPolyNormal (
    void*               bodyData,
    const API_Vector3D& normal,
    Int32&              index
);

// Add a polygon face (ordered edge indices, normal index, material).
// Edge indices sign direction: positive = forward, negative = reverse.
GSErrCode ACAPI_Body_AddPolygon (
    void*                           bodyData,
    const GS::Array<Int32>&         edges,
    Int32                           polyNormal,
    const API_OverriddenAttribute&  material,
    UInt32&                         index
);
```

### 12.3 Solid Operations (Boolean on Morphs)

```c
// Boolean operation between two existing Morph elements.
// resultGuids receives the GUIDs of created result elements.
GSErrCode ACAPI_Element_SolidOperation_Create (
    const API_Guid&         targetGuid,
    const API_Guid&         operatorGuid,
    API_SolidOperationID    operation,
    GS::Array<API_Guid>&    resultGuids
);

// Operation types (from APIdefs_Elements.h):
//   APISolid_Substract  ('SSUB')  — target minus operator
//   APISolid_SubstUp    ('SSUU')  — subtract upwards
//   APISolid_SubstDown  ('SSUD')  — subtract downwards
//   APISolid_Intersect  ('SINT')  — intersection
//   APISolid_Add        ('SADD')  — union

// Change the edge type of all edges on a Morph
GSErrCode ACAPI_Element_ChangeMorphEdgeType (
    const API_Guid&         morphGuid,
    API_MorphEdgeTypeID     newEdgeType
);
```

### 12.4 Morph Quantities

```c
struct API_MorphQuantity {
    double  surface;              // total surface area
    double  volume;               // volume
    double  floorPlanArea;        // floor-plan projected area
    double  floorPlanPerimeter;   // floor-plan perimeter
    double  baseLevel;            // lowest Z
    double  baseHeight;           // height above baseLevel
    double  wholeHeight;          // overall height
    Int32   nodesNr;              // vertex count
    Int32   edgesNr;              // edge count
    Int32   hiddenEdgesNr;
    Int32   softEdgesNr;
    Int32   visibleEdgesNr;
    Int32   facesNr;              // face count
};
// Access via: ACAPI_Element_GetQuantities(guid, nullptr, 0, APIScope_InCurrentPlan,
//               nullptr, &quantity, API_AbstractElemTypeID);
```

### 12.5 Workflow: Build Morph from Geometry

```
1. ACAPI_Element_GetDefaults(&element, &memo)          // default API_MorphType
2. Set element.morph.tranmat if needed
3. ACAPI_Body_Create(nullptr/existingBody, nullptr/matTable, &bodyData)
4. ACAPI_Body_AddVertex       (× N    — one per vertex)
5. ACAPI_Body_AddEdge         (× M    — one per edge)
6. ACAPI_Body_AddPolyNormal   (× K    — one per polygon normal)
7. ACAPI_Body_AddPolygon      (× K    — edges + normal + material per face)
8. ACAPI_Body_Finish(bodyData, &memo.morphBody, &memo.morphMaterialMapTable)
9. ACAPI_Body_Dispose(&bodyData)
10. ACAPI_Element_Create(&element, &memo)               // create the Morph
11. ACAPI_DisposeElemMemoHdls(&memo)

// Modify an existing Morph: get element + memo, pass memo.morphBody to
// ACAPI_Body_Create step 3, edit bodies, then ACAPI_Element_Change.
```

**Alternatively (import existing MeshBody):** if you already have a
`Modeler::MeshBody*` (e.g. from `API_ElementMemo.morphBody` of another
element, or from the C API model access), pass it directly to
`ACAPI_Body_Create` at step 3. The Body API clones the input geometry
so the original MeshBody is not consumed.
