/**
 * "Rename this node" / "open this node's quick menu", from somewhere that
 * cannot reach the node itself.
 *
 * The canvas context menu lives in the editor and both of those live in the
 * node, so the menu asks and the node answers. One shared request rather than a
 * second rename UI in the menu: a person who renames from the menu and a person
 * who right-clicks the title bar should land in the same field, or the two
 * drift apart the first time one of them grows a rule.
 *
 * It also keeps the quick menu reachable without a middle button, which a
 * trackpad may not have.
 */
export type NodeRequestKind = 'rename' | 'menu'

const request = $state<{ nodeId: string | null; kind: NodeRequestKind }>({ nodeId: null, kind: 'rename' })

export function requestRename(nodeId: string): void {
  request.nodeId = nodeId
  request.kind = 'rename'
}

export function requestQuickMenu(nodeId: string): void {
  request.nodeId = nodeId
  request.kind = 'menu'
}

/** The pending request's kind for this node, or undefined if it is not for it. */
export function pendingRequest(nodeId: string): NodeRequestKind | undefined {
  return request.nodeId === nodeId ? request.kind : undefined
}

export function clearRequest(): void {
  request.nodeId = null
}
