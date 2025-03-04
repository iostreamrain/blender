# SPDX-License-Identifier: GPL-2.0-or-later
import bpy
from bpy.types import Menu
from bl_ui import node_add_menu
from bpy.app.translations import pgettext_iface as iface_


class NODE_MT_geometry_node_GEO_ATTRIBUTE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_ATTRIBUTE"
    bl_label = "Attribute"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeAttributeStatistic")
        node_add_menu.add_node_type(layout, "GeometryNodeCaptureAttribute")
        node_add_menu.add_node_type(layout, "GeometryNodeAttributeDomainSize")
        node_add_menu.add_node_type(layout, "GeometryNodeRemoveAttribute")
        node_add_menu.add_node_type(layout, "GeometryNodeStoreNamedAttribute")


class NODE_MT_geometry_node_GEO_COLOR(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_COLOR"
    bl_label = "Color"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "ShaderNodeValToRGB")
        node_add_menu.add_node_type(layout, "FunctionNodeCombineColor")
        props = node_add_menu.add_node_type(layout, "ShaderNodeMix", label=iface_("Mix Color"))
        ops = props.settings.add()
        ops.name = "data_type"
        ops.value = "'RGBA'"
        node_add_menu.add_node_type(layout, "ShaderNodeRGBCurve")
        node_add_menu.add_node_type(layout, "FunctionNodeSeparateColor")


class NODE_MT_geometry_node_GEO_CURVE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_CURVE"
    bl_label = "Curve"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeCurveLength")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveToMesh")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveToPoints")
        node_add_menu.add_node_type(layout, "GeometryNodeDeformCurvesOnSurface")
        node_add_menu.add_node_type(layout, "GeometryNodeFillCurve")
        node_add_menu.add_node_type(layout, "GeometryNodeFilletCurve")
        node_add_menu.add_node_type(layout, "GeometryNodeResampleCurve")
        node_add_menu.add_node_type(layout, "GeometryNodeReverseCurve")
        node_add_menu.add_node_type(layout, "GeometryNodeSampleCurve")
        node_add_menu.add_node_type(layout, "GeometryNodeSubdivideCurve")
        node_add_menu.add_node_type(layout, "GeometryNodeTrimCurve")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeInputCurveHandlePositions")
        node_add_menu.add_node_type(layout, "GeometryNodeInputTangent")
        node_add_menu.add_node_type(layout, "GeometryNodeInputCurveTilt")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveEndpointSelection")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveHandleTypeSelection")
        node_add_menu.add_node_type(layout, "GeometryNodeInputSplineCyclic")
        node_add_menu.add_node_type(layout, "GeometryNodeSplineLength")
        node_add_menu.add_node_type(layout, "GeometryNodeSplineParameter")
        node_add_menu.add_node_type(layout, "GeometryNodeInputSplineResolution")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeSetCurveRadius")
        node_add_menu.add_node_type(layout, "GeometryNodeSetCurveTilt")
        node_add_menu.add_node_type(layout, "GeometryNodeSetCurveHandlePositions")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveSetHandles")
        node_add_menu.add_node_type(layout, "GeometryNodeSetSplineCyclic")
        node_add_menu.add_node_type(layout, "GeometryNodeSetSplineResolution")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveSplineType")


class NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE"
    bl_label = "Curve Primitives"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeCurveArc")
        node_add_menu.add_node_type(layout, "GeometryNodeCurvePrimitiveBezierSegment")
        node_add_menu.add_node_type(layout, "GeometryNodeCurvePrimitiveCircle")
        node_add_menu.add_node_type(layout, "GeometryNodeCurvePrimitiveLine")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveSpiral")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveQuadraticBezier")
        node_add_menu.add_node_type(layout, "GeometryNodeCurvePrimitiveQuadrilateral")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveStar")


class NODE_MT_geometry_node_curve_topology(Menu):
    bl_idname = "NODE_MT_geometry_node_curve_topology"
    bl_label = "Curve Topology"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeInputControlPointNeighbors")
        node_add_menu.add_node_type(layout, "GeometryNodeCurveOfPoint")
        node_add_menu.add_node_type(layout, "GeometryNodePointsOfCurve")


class NODE_MT_geometry_node_GEO_GEOMETRY(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_GEOMETRY"
    bl_label = "Geometry"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeBoundBox")
        node_add_menu.add_node_type(layout, "GeometryNodeConvexHull")
        node_add_menu.add_node_type(layout, "GeometryNodeDeleteGeometry")
        node_add_menu.add_node_type(layout, "GeometryNodeDuplicateElements")
        node_add_menu.add_node_type(layout, "GeometryNodeProximity")
        node_add_menu.add_node_type(layout, "GeometryNodeGeometryToInstance")
        node_add_menu.add_node_type(layout, "GeometryNodeJoinGeometry")
        node_add_menu.add_node_type(layout, "GeometryNodeMergeByDistance")
        node_add_menu.add_node_type(layout, "GeometryNodeRaycast")
        node_add_menu.add_node_type(layout, "GeometryNodeSampleIndex")
        node_add_menu.add_node_type(layout, "GeometryNodeSampleNearest")
        node_add_menu.add_node_type(layout, "GeometryNodeSeparateComponents")
        node_add_menu.add_node_type(layout, "GeometryNodeSeparateGeometry")
        node_add_menu.add_node_type(layout, "GeometryNodeTransform")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeSetID")
        node_add_menu.add_node_type(layout, "GeometryNodeSetPosition")


class NODE_MT_geometry_node_GEO_INPUT(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_INPUT"
    bl_label = "Input"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "FunctionNodeInputBool")
        node_add_menu.add_node_type(layout, "GeometryNodeCollectionInfo")
        node_add_menu.add_node_type(layout, "FunctionNodeInputColor")
        node_add_menu.add_node_type(layout, "FunctionNodeInputInt")
        node_add_menu.add_node_type(layout, "GeometryNodeIsViewport")
        node_add_menu.add_node_type(layout, "GeometryNodeInputMaterial")
        node_add_menu.add_node_type(layout, "GeometryNodeObjectInfo")
        node_add_menu.add_node_type(layout, "GeometryNodeSelfObject")
        node_add_menu.add_node_type(layout, "FunctionNodeInputString")
        node_add_menu.add_node_type(layout, "ShaderNodeValue")
        node_add_menu.add_node_type(layout, "FunctionNodeInputVector")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeInputID")
        node_add_menu.add_node_type(layout, "GeometryNodeInputIndex")
        node_add_menu.add_node_type(layout, "GeometryNodeInputNamedAttribute")
        node_add_menu.add_node_type(layout, "GeometryNodeInputNormal")
        node_add_menu.add_node_type(layout, "GeometryNodeInputPosition")
        node_add_menu.add_node_type(layout, "GeometryNodeInputRadius")
        node_add_menu.add_node_type(layout, "GeometryNodeInputSceneTime")


class NODE_MT_geometry_node_GEO_INSTANCE(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_INSTANCE"
    bl_label = "Instances"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeInstanceOnPoints")
        node_add_menu.add_node_type(layout, "GeometryNodeInstancesToPoints")
        node_add_menu.add_node_type(layout, "GeometryNodeRealizeInstances")
        node_add_menu.add_node_type(layout, "GeometryNodeRotateInstances")
        node_add_menu.add_node_type(layout, "GeometryNodeScaleInstances")
        node_add_menu.add_node_type(layout, "GeometryNodeTranslateInstances")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeInputInstanceRotation")
        node_add_menu.add_node_type(layout, "GeometryNodeInputInstanceScale")


class NODE_MT_geometry_node_GEO_MATERIAL(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_MATERIAL"
    bl_label = "Material"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeReplaceMaterial")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeInputMaterialIndex")
        node_add_menu.add_node_type(layout, "GeometryNodeMaterialSelection")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeSetMaterial")
        node_add_menu.add_node_type(layout, "GeometryNodeSetMaterialIndex")


class NODE_MT_geometry_node_GEO_MESH(Menu):
    bl_idname = "NODE_MT_geometry_node_GEO_MESH"
    bl_label = "Mesh"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeDualMesh")
        node_add_menu.add_node_type(layout, "GeometryNodeEdgePathsToCurves")
        node_add_menu.add_node_type(layout, "GeometryNodeEdgePathsToSelection")
        node_add_menu.add_node_type(layout, "GeometryNodeExtrudeMesh")
        node_add_menu.add_node_type(layout, "GeometryNodeFlipFaces")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshBoolean")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshToCurve")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshToPoints")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshToVolume")
        node_add_menu.add_node_type(layout, "GeometryNodeSampleNearestSurface")
        node_add_menu.add_node_type(layout, "GeometryNodeScaleElements")
        node_add_menu.add_node_type(layout, "GeometryNodeSplitEdges")
        node_add_menu.add_node_type(layout, "GeometryNodeSubdivideMesh")
        node_add_menu.add_node_type(layout, "GeometryNodeSubdivisionSurface")
        node_add_menu.add_node_type(layout, "GeometryNodeTriangulate")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshEdgeAngle")
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshEdgeNeighbors")
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshEdgeVertices")
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshFaceArea")
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshFaceNeighbors")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshFaceSetBoundaries")
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshFaceIsPlanar")
        node_add_menu.add_node_type(layout, "GeometryNodeInputShadeSmooth")
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshIsland")
        node_add_menu.add_node_type(layout, "GeometryNodeInputShortestEdgePaths")
        node_add_menu.add_node_type(layout, "GeometryNodeInputMeshVertexNeighbors")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeSetShadeSmooth")


class NODE_MT_category_PRIMITIVES_MESH(Menu):
    bl_idname = "NODE_MT_category_PRIMITIVES_MESH"
    bl_label = "Mesh Primitives"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeMeshCone")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshCube")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshCylinder")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshGrid")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshIcoSphere")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshCircle")
        node_add_menu.add_node_type(layout, "GeometryNodeMeshLine")


class NODE_MT_geometry_node_mesh_topology(Menu):
    bl_idname = "NODE_MT_geometry_node_mesh_topology"
    bl_label = "Mesh Topology"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeCornersOfFace"),
        node_add_menu.add_node_type(layout, "GeometryNodeCornersOfVertex"),
        node_add_menu.add_node_type(layout, "GeometryNodeEdgesOfCorner"),
        node_add_menu.add_node_type(layout, "GeometryNodeEdgesOfVertex"),
        node_add_menu.add_node_type(layout, "GeometryNodeFaceOfCorner"),
        node_add_menu.add_node_type(layout, "GeometryNodeOffsetCornerInFace"),
        node_add_menu.add_node_type(layout, "GeometryNodeVertexOfCorner"),


class NODE_MT_category_GEO_OUTPUT(Menu):
    bl_idname = "NODE_MT_category_GEO_OUTPUT"
    bl_label = "Output"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeViewer")


class NODE_MT_category_GEO_POINT(Menu):
    bl_idname = "NODE_MT_category_GEO_POINT"
    bl_label = "Point"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeDistributePointsInVolume")
        node_add_menu.add_node_type(layout, "GeometryNodeDistributePointsOnFaces")
        node_add_menu.add_node_type(layout, "GeometryNodePoints")
        node_add_menu.add_node_type(layout, "GeometryNodePointsToVertices")
        node_add_menu.add_node_type(layout, "GeometryNodePointsToVolume")
        layout.separator()
        node_add_menu.add_node_type(layout, "GeometryNodeSetPointRadius")


class NODE_MT_category_GEO_TEXT(Menu):
    bl_idname = "NODE_MT_category_GEO_TEXT"
    bl_label = "Text"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeStringJoin")
        node_add_menu.add_node_type(layout, "FunctionNodeReplaceString")
        node_add_menu.add_node_type(layout, "FunctionNodeSliceString")
        node_add_menu.add_node_type(layout, "FunctionNodeStringLength")
        node_add_menu.add_node_type(layout, "GeometryNodeStringToCurves")
        node_add_menu.add_node_type(layout, "FunctionNodeValueToString")
        layout.separator()
        node_add_menu.add_node_type(layout, "FunctionNodeInputSpecialCharacters")


class NODE_MT_category_GEO_TEXTURE(Menu):
    bl_idname = "NODE_MT_category_GEO_TEXTURE"
    bl_label = "Texture"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "ShaderNodeTexBrick")
        node_add_menu.add_node_type(layout, "ShaderNodeTexChecker")
        node_add_menu.add_node_type(layout, "ShaderNodeTexGradient")
        node_add_menu.add_node_type(layout, "GeometryNodeImageTexture")
        node_add_menu.add_node_type(layout, "ShaderNodeTexMagic")
        node_add_menu.add_node_type(layout, "ShaderNodeTexMusgrave")
        node_add_menu.add_node_type(layout, "ShaderNodeTexNoise")
        node_add_menu.add_node_type(layout, "ShaderNodeTexVoronoi")
        node_add_menu.add_node_type(layout, "ShaderNodeTexWave")
        node_add_menu.add_node_type(layout, "ShaderNodeTexWhiteNoise")


class NODE_MT_category_GEO_UTILITIES(Menu):
    bl_idname = "NODE_MT_category_GEO_UTILITIES"
    bl_label = "Utilities"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeAccumulateField")
        node_add_menu.add_node_type(layout, "FunctionNodeAlignEulerToVector")
        node_add_menu.add_node_type(layout, "FunctionNodeBooleanMath")
        node_add_menu.add_node_type(layout, "ShaderNodeClamp")
        node_add_menu.add_node_type(layout, "FunctionNodeCompare")
        node_add_menu.add_node_type(layout, "GeometryNodeFieldAtIndex")
        node_add_menu.add_node_type(layout, "ShaderNodeFloatCurve")
        node_add_menu.add_node_type(layout, "FunctionNodeFloatToInt")
        node_add_menu.add_node_type(layout, "GeometryNodeFieldOnDomain")
        node_add_menu.add_node_type(layout, "ShaderNodeMapRange")
        node_add_menu.add_node_type(layout, "ShaderNodeMath")
        node_add_menu.add_node_type(layout, "ShaderNodeMix")
        node_add_menu.add_node_type(layout, "FunctionNodeRandomValue")
        node_add_menu.add_node_type(layout, "FunctionNodeRotateEuler")
        node_add_menu.add_node_type(layout, "GeometryNodeSwitch")


class NODE_MT_category_GEO_UV(Menu):
    bl_idname = "NODE_MT_category_GEO_UV"
    bl_label = "UV"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeUVPackIslands")
        node_add_menu.add_node_type(layout, "GeometryNodeUVUnwrap")


class NODE_MT_category_GEO_VECTOR(Menu):
    bl_idname = "NODE_MT_category_GEO_VECTOR"
    bl_label = "Vector"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "ShaderNodeCombineXYZ")
        node_add_menu.add_node_type(layout, "ShaderNodeSeparateXYZ")
        node_add_menu.add_node_type(layout, "ShaderNodeVectorCurve")
        node_add_menu.add_node_type(layout, "ShaderNodeVectorMath")
        node_add_menu.add_node_type(layout, "ShaderNodeVectorRotate")


class NODE_MT_category_GEO_VOLUME(Menu):
    bl_idname = "NODE_MT_category_GEO_VOLUME"
    bl_label = "Volume"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "GeometryNodeVolumeCube")
        node_add_menu.add_node_type(layout, "GeometryNodeVolumeToMesh")


class NODE_MT_category_GEO_GROUP(Menu):
    bl_idname = "NODE_MT_category_GEO_GROUP"
    bl_label = "Group"

    def draw(self, context):
        layout = self.layout
        node_add_menu.draw_node_group_add_menu(context, layout)


class NODE_MT_category_GEO_LAYOUT(Menu):
    bl_idname = "NODE_MT_category_GEO_LAYOUT"
    bl_label = "Layout"

    def draw(self, _context):
        layout = self.layout
        node_add_menu.add_node_type(layout, "NodeFrame")
        node_add_menu.add_node_type(layout, "NodeReroute")


class NODE_MT_geometry_node_add_all(Menu):
    bl_idname = "NODE_MT_geometry_node_add_all"
    bl_label = ""

    def draw(self, _context):
        layout = self.layout
        layout.menu("NODE_MT_geometry_node_GEO_ATTRIBUTE")
        layout.menu("NODE_MT_geometry_node_GEO_COLOR")
        layout.menu("NODE_MT_geometry_node_GEO_CURVE")
        layout.menu("NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE")
        layout.menu("NODE_MT_geometry_node_curve_topology")
        layout.menu("NODE_MT_geometry_node_GEO_GEOMETRY")
        layout.menu("NODE_MT_geometry_node_GEO_INPUT")
        layout.menu("NODE_MT_geometry_node_GEO_INSTANCE")
        layout.menu("NODE_MT_geometry_node_GEO_MATERIAL")
        layout.menu("NODE_MT_geometry_node_GEO_MESH")
        layout.menu("NODE_MT_category_PRIMITIVES_MESH")
        layout.menu("NODE_MT_geometry_node_mesh_topology")
        layout.menu("NODE_MT_category_GEO_OUTPUT")
        layout.menu("NODE_MT_category_GEO_POINT")
        layout.menu("NODE_MT_category_GEO_TEXT")
        layout.menu("NODE_MT_category_GEO_TEXTURE")
        layout.menu("NODE_MT_category_GEO_UTILITIES")
        layout.menu("NODE_MT_category_GEO_UV")
        layout.menu("NODE_MT_category_GEO_VECTOR")
        layout.menu("NODE_MT_category_GEO_VOLUME")
        layout.menu("NODE_MT_category_GEO_GROUP")
        layout.menu("NODE_MT_category_GEO_LAYOUT")


classes = (
    NODE_MT_geometry_node_add_all,
    NODE_MT_geometry_node_GEO_ATTRIBUTE,
    NODE_MT_geometry_node_GEO_COLOR,
    NODE_MT_geometry_node_GEO_CURVE,
    NODE_MT_geometry_node_GEO_PRIMITIVES_CURVE,
    NODE_MT_geometry_node_curve_topology,
    NODE_MT_geometry_node_GEO_GEOMETRY,
    NODE_MT_geometry_node_GEO_INPUT,
    NODE_MT_geometry_node_GEO_INSTANCE,
    NODE_MT_geometry_node_GEO_MATERIAL,
    NODE_MT_geometry_node_GEO_MESH,
    NODE_MT_category_PRIMITIVES_MESH,
    NODE_MT_geometry_node_mesh_topology,
    NODE_MT_category_GEO_OUTPUT,
    NODE_MT_category_GEO_POINT,
    NODE_MT_category_GEO_TEXT,
    NODE_MT_category_GEO_TEXTURE,
    NODE_MT_category_GEO_UTILITIES,
    NODE_MT_category_GEO_UV,
    NODE_MT_category_GEO_VECTOR,
    NODE_MT_category_GEO_VOLUME,
    NODE_MT_category_GEO_GROUP,
    NODE_MT_category_GEO_LAYOUT,
)

if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
