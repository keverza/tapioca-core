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
    resizable: viewer || schema.parameters.length > 2 || schema.display === 'text',
    nodeViewer: viewer,
    canvasPreview: false,
    overlayPreview: false,
    floatingViewer: false,
    floatingControls: schema.parameters.length > 0,
    gumball: false,
  }
}

export function presentationFor(schema: NodeTypeSchema): NodePresentation {
  const capabilities = capabilitiesFor(schema)
  return {
    bodyMode: bodyModeFor(schema),
    portLayout: 'horizontal',
    resizable: capabilities.resizable,
    minSize: { width: 220, height: 80 },
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
