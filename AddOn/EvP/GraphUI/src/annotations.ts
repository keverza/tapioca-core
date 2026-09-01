import type { Node, XYPosition } from '@xyflow/svelte'
import type { SchemaNodeData } from './types'

export type EditorTool = 'select' | 'eraser' | 'rectangle'
export type EffectiveTool = EditorTool | 'knife'

export interface AnnotationBounds {
  x: number
  y: number
  width: number
  height: number
}

export interface EditorAnnotation {
  id: string
  kind: 'rectangle' | 'frame'
  bounds: AnnotationBounds
  label: string
  memberNodeIds: string[]
}

interface StoredEditorMetadata {
  version: 1
  annotations: EditorAnnotation[]
}

const STORAGE_PREFIX = 'tapioca.graph.editor.'
const NODE_WIDTH = 228
const NODE_HEIGHT = 160
const FRAME_PADDING = 48

export function boundsFromPoints(start: XYPosition, end: XYPosition): AnnotationBounds {
  return {
    x: Math.min(start.x, end.x),
    y: Math.min(start.y, end.y),
    width: Math.abs(end.x - start.x),
    height: Math.abs(end.y - start.y),
  }
}

export function annotationFromSelection(
  id: string,
  nodes: Node<SchemaNodeData>[],
): EditorAnnotation | undefined {
  if (nodes.length === 0) return undefined
  const left = Math.min(...nodes.map((node) => node.position.x)) - FRAME_PADDING
  const top = Math.min(...nodes.map((node) => node.position.y)) - FRAME_PADDING
  const right = Math.max(...nodes.map((node) => node.position.x + NODE_WIDTH)) + FRAME_PADDING
  const bottom = Math.max(...nodes.map((node) => node.position.y + NODE_HEIGHT)) + FRAME_PADDING
  return {
    id,
    kind: 'frame',
    bounds: { x: left, y: top, width: right - left, height: bottom - top },
    label: 'Subflow frame',
    memberNodeIds: nodes.map((node) => node.id),
  }
}

export function resolveFrameBounds(
  annotation: EditorAnnotation,
  nodes: Node<SchemaNodeData>[],
): EditorAnnotation {
  if (annotation.kind !== 'frame') return annotation
  const members = nodes.filter((node) => annotation.memberNodeIds.includes(node.id))
  const resolved = annotationFromSelection(annotation.id, members)
  return resolved === undefined ? annotation : { ...resolved, label: annotation.label }
}

/** Remove one annotation by id. A no-op for an id that is not there. */
export function removeAnnotation(
  annotations: EditorAnnotation[],
  id: string,
): EditorAnnotation[] {
  return annotations.filter((annotation) => annotation.id !== id)
}

/**
 * Rename one annotation.
 *
 * An empty name is kept as an empty label rather than refused: a rectangle with
 * no caption is a legitimate thing to want, and it is also how a caption is
 * removed once it has been typed.
 */
export function renameAnnotation(
  annotations: EditorAnnotation[],
  id: string,
  label: string,
): EditorAnnotation[] {
  return annotations.map((annotation) =>
    annotation.id === id ? { ...annotation, label: label.trim() } : annotation,
  )
}

/** The topmost annotation containing a flow-space point, or undefined. */
export function annotationAtPoint(
  annotations: EditorAnnotation[],
  point: XYPosition,
): EditorAnnotation | undefined {
  // Last first: later annotations are drawn on top, so they are hit first.
  for (let index = annotations.length - 1; index >= 0; index -= 1) {
    const { bounds } = annotations[index]
    if (
      point.x >= bounds.x &&
      point.x <= bounds.x + bounds.width &&
      point.y >= bounds.y &&
      point.y <= bounds.y + bounds.height
    ) {
      return annotations[index]
    }
  }
  return undefined
}

export function loadAnnotations(
  storage: Pick<Storage, 'getItem'> | undefined,
  graphKey: string,
): EditorAnnotation[] {
  const encoded = storage?.getItem(`${STORAGE_PREFIX}${graphKey}`)
  if (encoded === null || encoded === undefined) return []
  try {
    const stored = JSON.parse(encoded) as Partial<StoredEditorMetadata>
    if (stored.version !== 1 || !Array.isArray(stored.annotations)) return []
    return stored.annotations.filter(isAnnotation)
  } catch {
    return []
  }
}

export function saveAnnotations(
  storage: Pick<Storage, 'setItem'>,
  graphKey: string,
  annotations: EditorAnnotation[],
): void {
  const metadata: StoredEditorMetadata = { version: 1, annotations }
  storage.setItem(`${STORAGE_PREFIX}${graphKey}`, JSON.stringify(metadata))
}

function isAnnotation(value: unknown): value is EditorAnnotation {
  if (typeof value !== 'object' || value === null) return false
  const annotation = value as Partial<EditorAnnotation>
  const bounds = annotation.bounds as Partial<AnnotationBounds> | undefined
  return (
    typeof annotation.id === 'string' &&
    (annotation.kind === 'rectangle' || annotation.kind === 'frame') &&
    typeof annotation.label === 'string' &&
    Array.isArray(annotation.memberNodeIds) &&
    bounds !== undefined &&
    Number.isFinite(bounds.x) &&
    Number.isFinite(bounds.y) &&
    Number.isFinite(bounds.width) &&
    Number.isFinite(bounds.height)
  )
}
