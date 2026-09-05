import type { NodeTypeSchema } from '../types'
import { bodyModeFor } from './types/display.ts'
import type { NodeCapabilities, NodeDefinition, NodePresentation } from './types/node'
import type { PortDefinition } from './types/port'

export const NODE_DEFINITION_VERSION = 1

function portDefinition(port: NodeTypeSchema['inputs'][number], direction: 'input' | 'output'): PortDefinition {
  return {
    ...port,
    capabilities: {
      copyReference: true,
      pasteReference: direction === 'input',
      internalise: direction === 'input',
      promote: true,
      inspect: true,
      transforms: direction === 'input' ? ['reverse', 'simplify', 'flatten', 'graft', 'reparameterize'] : ['simplify', 'flatten', 'graft'],
    },
  }
}

export function capabilitiesFor(schema: NodeTypeSchema): NodeCapabilities {
  const viewer = schema.display === 'preview'
  return {
    bypass: (schema.bypassMappings?.length ?? 0) > 0,
    disable: true,
    freeze: schema.holdCapable === true,
    /**
     * ⚠️ ONLY THE NODES WHOSE CONTENT HAS A SIZE. This used to include any node
     * with more than two parameters, which handed a resize frame to every
     * slider and picker on the canvas - nodes whose height is entirely decided
     * by their rows. The visible cost was a resize outline flickering around a
     * Number node while its slider was being dragged, but the real cost is that
     * a resizable node invites a size it cannot use.
     *
     * What is left is the two bodies that genuinely hold a variable amount of
     * something: a preview's viewport and a panel's text.
     *
     * ⚠️ AND A SCRIPT NODE IS NOT ONE OF THEM, THOUGH IT USED TO BE. Nothing in
     * a script node's body has a size: it is a status line, a name and a row of
     * icons, all of which are as tall as they are going to get. The editor - the
     * one part that genuinely wants room - is not drawn on the node at all; it is
     * the Script Inspector, which takes the canvas's full height and is resized
     * on its own edge. Leaving the frame on the node offered a size that could
     * only add empty space, on the node family that already carries the most
     * controls in the smallest area.
     */
    resizable: viewer || schema.display === 'text',
    /**
     * ⚠️ ONLY EFFECTFUL NODES GET THE BUTTON, AND EVERY EFFECTFUL NODE GETS IT.
     * Automatic evaluation runs the graph continuously and NEVER commits a host
     * effect - `allowSideEffects` defaults to refused, so an effectful node is
     * reported as skipped on every automatic pass. This button is the deliberate
     * act that commits it. Keyed on the catalog's `effect`, so a node type added
     * later gets its button with no change here; recognising particular node ids
     * is the branch this whole contract layer exists to remove.
     */
    execute: schema.effect === 'hostUiWrite',
    nodeViewer: viewer,
    canvasPreview: false,
    overlayPreview: false,
    floatingViewer: false,
    floatingControls: schema.parameters.length > 0,
    gumball: false,
  }
}

/**
 * The smallest a node may be dragged to, by what its body holds.
 *
 * Header, ports and padding come to roughly 70px before the body draws
 * anything at all, which is why every floor here is well above it.
 */
function minimumSizeFor(schema: NodeTypeSchema, capabilities: NodeCapabilities): { width: number; height: number } {
  if (capabilities.nodeViewer) return { width: 240, height: 200 }
  if (schema.display === 'script') return { width: 280, height: 180 }
  if (schema.display === 'text') return { width: 220, height: 140 }
  return { width: 220, height: 110 }
}

export function presentationFor(schema: NodeTypeSchema): NodePresentation {
  const capabilities = capabilitiesFor(schema)
  return {
    bodyMode: bodyModeFor(schema),
    portLayout: 'horizontal',
    resizable: capabilities.resizable,
    /**
     * The floor a resize may not go under.
     *
     * ⚠️ NOT ONE NUMBER FOR EVERY BODY. 80px was below the height these bodies
     * actually occupy, so dragging the frame in pushed the content out through
     * the node's own edges - a viewport clipped to a sliver, a code editor with
     * no room for a line. The minimum is what the body needs to draw itself: a
     * viewport wants room to be a viewport, and text wants more than a header.
     */
    minSize: minimumSizeFor(schema, capabilities),
    maxSize: { width: 720, height: 640 },
    controls: {
      inline: schema.parameters.length > 0,
      expandable: schema.parameters.length > 0,
      detachable: capabilities.floatingControls,
    },
    viewer: {
      enabled: capabilities.nodeViewer,
      detachable: capabilities.floatingViewer,
      navigation: capabilities.nodeViewer,
      gumball: capabilities.gumball,
    },
  }
}

export function definitionFromSchema(schema: NodeTypeSchema): NodeDefinition {
  return {
    ...schema,
    version: NODE_DEFINITION_VERSION,
    inputs: schema.inputs.map((port) => portDefinition(port, 'input')),
    outputs: schema.outputs.map((port) => portDefinition(port, 'output')),
    presentation: presentationFor(schema),
    capabilities: capabilitiesFor(schema),
  }
}
