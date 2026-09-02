import type { GraphNodeRecord, NodeTypeSchema } from '../../types'

/**
 * The schema to draw a NODE with, as opposed to the schema of its TYPE.
 *
 * ⚠️ THE ONE SEAM FOR PER-NODE PORTS, AND EVERY READER WITH A NODE IN HAND MUST
 * GO THROUGH IT. For all but one family these are the same object and this
 * function is a pass-through. For a script node they are not: its ports are
 * declared in the file it runs, so the catalog entry carries none and the
 * per-node state carries them instead.
 *
 * The failure this exists to prevent is quiet. A reader that keeps using
 * `catalog.get(node.nodeType)` on a script node gets a schema with empty
 * `inputs` and `outputs`, so the node renders with no handles, its edges have
 * nowhere to attach, and connection validation refuses every wire - all of which
 * looks exactly like a broken node rather than like a caller that skipped a
 * function. It mirrors ResolvedInputs/ResolvedOutputs on the native side, which
 * exists for the same reason.
 */
export function schemaForNode(node: GraphNodeRecord, schema: NodeTypeSchema | undefined): NodeTypeSchema | undefined {
  if (schema === undefined) return undefined
  if (schema.instancePorts !== true) return schema
  return { ...schema, inputs: node.inputs ?? [], outputs: node.outputs ?? [] }
}

/**
 * The same merge, for the places that hold a node id rather than the record.
 * Returns undefined when the graph has no such node, which a caller must treat
 * as "not drawable yet" rather than as "no ports".
 */
export function schemaForNodeId(
  nodeId: string,
  nodes: GraphNodeRecord[],
  catalog: Map<string, NodeTypeSchema>,
): NodeTypeSchema | undefined {
  const node = nodes.find((candidate) => candidate.nodeId === nodeId)
  if (node === undefined) return undefined
  return schemaForNode(node, catalog.get(node.nodeType))
}
