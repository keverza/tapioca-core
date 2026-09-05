/**
 * The per-type containers a selection node stacks, and the grouped settings tree
 * a container expands into. Pure, so both are testable without a bridge.
 *
 * ⚠️ NEITHER FUNCTION CARRIES A COPY OF WHAT AN ARCHICAD ELEMENT IS. The type
 * order and the group labels come from the catalog the runtime shipped with
 * `GraphGetNodeTypes`; the setting labels, groups and order come from the same
 * response that carried the values. A table restated here is the one that goes
 * stale after a build and starts showing a thickness under the label "Height",
 * with nothing to say it happened.
 */
import type {
  ElementDescription,
  ElementGroup,
  ElementSettingSchema,
  ElementSettingValue,
  ElementTypeSchema,
  GraphParameter,
  GraphValue,
} from '../../types'

/** The catalog's id for anything the runtime does not classify. */
export const UNCLASSIFIED_ELEMENT_TYPE = 'other'

/** Just enough of the catalog to order and name a stack. */
export interface ElementTypeInfo {
  id: string
  label: string
  plural: string
  container: boolean
}

function stringItems(parameter: GraphParameter | undefined): string[] {
  return (parameter?.value?.items ?? []).map((item) => item.text ?? '')
}

/**
 * The stack for a selection node, from its two stored parameters.
 *
 * ⚠️ THE TWO LISTS ARE PARALLEL AND THE SHORTER ONE DOES NOT SHIFT ANYTHING. A
 * type list shorter than the guids - a truncated encoding, a hand-edited file -
 * leaves the remainder unclassified. Filing those under the last known type
 * instead would produce a stack that adds up correctly and is wrong about every
 * element in it, which is the failure nobody would notice.
 */
export function elementGroupsOf(parameters: GraphParameter[], catalog: ElementTypeInfo[]): ElementGroup[] {
  const elements = parameters.find((parameter) => parameter.parameterId === 'elements')
  const types = parameters.find((parameter) => parameter.parameterId === 'elementTypes')

  const guids = (elements?.value?.items ?? []).map((item) => item.text ?? '')
  const capturedTypes = stringItems(types)
  const known = new Set(catalog.map((type) => type.id))

  const byType = new Map<string, string[]>()
  guids.forEach((guid, index) => {
    const captured = capturedTypes[index] ?? ''
    const id = known.has(captured) ? captured : UNCLASSIFIED_ELEMENT_TYPE
    const bucket = byType.get(id)
    if (bucket === undefined) byType.set(id, [guid])
    else bucket.push(guid)
  })

  // Catalog order, not selection order: the same model clicked in a different
  // order has to produce the same panel.
  const groups: ElementGroup[] = []
  for (const type of catalog) {
    const held = byType.get(type.id)
    if (held === undefined || held.length === 0) continue
    groups.push({ elementType: type.id, label: type.plural, guids: held })
  }
  return groups
}

/**
 * How many elements the stack accounts for. Shown beside the node's own count so
 * a disagreement between them is VISIBLE rather than being quietly absorbed:
 * they differ exactly when the encoding truncated the stored list.
 */
export function stackedCount(groups: ElementGroup[]): number {
  return groups.reduce((total, group) => total + group.guids.length, 0)
}

export interface SettingRow {
  id: string
  label: string
  unit: string
  text: string
  /** 'archicad' or 'derived' - see ElementSettingSchema.origin. */
  origin: string
}

/**
 * Whether a descriptor applies to THIS element at all.
 *
 * ⚠️ NOT THE SAME QUESTION AS "DID THE READER FILL IT". A Basic wall has no
 * composite: the row does not apply, and the reader correctly wrote nothing.
 * An unread row is one that DOES apply and is still missing. Conflating them
 * makes a complete panel report itself as short - see `unreadSettingCount`.
 *
 * The condition is the runtime's, carried in the same response as the values,
 * and it is compared against the sibling's RENDERED TEXT because that is the
 * only form of the sibling a client is given.
 */
function applies(descriptor: ElementSettingSchema, values: Map<string, ElementSettingValue>): boolean {
  if (descriptor.appliesWhenSetting === '') return true
  const governing = values.get(descriptor.appliesWhenSetting)
  // ⚠️ AN UNREAD GOVERNING SETTING MAKES THE ROW INAPPLICABLE, NOT APPLICABLE.
  // If the build could not read what structure a wall is, it cannot know which
  // of the three material rows to expect - and reporting all three as unread
  // would blame the reader for three fields on the strength of one.
  if (governing === undefined) return false
  return governing.text === descriptor.appliesWhenEquals
}

export interface SettingSection {
  group: string
  rows: SettingRow[]
}

/**
 * One element's settings, in the runtime's order, split into its groups.
 *
 * ⚠️ A SETTING THE RUNTIME DID NOT SEND IS OMITTED, NOT SHOWN AS EMPTY. Absence
 * means this build could not read that field for this element; drawing it as a
 * blank row would make an unread field and a field that is genuinely blank look
 * identical, and the caller has no way to tell them apart afterwards.
 */
export function settingSectionsOf(
  element: ElementDescription,
  schema: ElementTypeSchema | undefined,
): SettingSection[] {
  if (schema === undefined) return []
  const values = new Map<string, ElementSettingValue>(element.settings.map((setting) => [setting.id, setting]))

  const sections: SettingSection[] = []
  for (const descriptor of schema.settings) {
    const value = values.get(descriptor.id)
    if (value === undefined) continue
    let section = sections.find((candidate) => candidate.group === descriptor.group)
    if (section === undefined) {
      section = { group: descriptor.group, rows: [] }
      sections.push(section)
    }
    section.rows.push({
      id: descriptor.id,
      label: descriptor.label,
      unit: descriptor.unit,
      text: value.text,
      origin: descriptor.origin,
    })
  }
  return sections
}

/**
 * How many of a type's settings this build could NOT read for this element.
 * Reported rather than hidden: "a wall has fourteen settings and you are seeing
 * eleven" is a fact the user should have, and it is the only signal that the
 * ACAPI reader has not caught up with the table.
 */
export function unreadSettingCount(
  element: ElementDescription,
  schema: ElementTypeSchema | undefined,
): number {
  if (schema === undefined) return 0
  const values = new Map<string, ElementSettingValue>(element.settings.map((setting) => [setting.id, setting]))
  // ⚠️ ROWS THAT DO NOT APPLY ARE NOT MISSING ROWS. A Basic wall declares a
  // Composite and a Profile it cannot have; counting those made every wall in
  // the model report two settings "this build does not read yet", which is a
  // complaint about the reader for something the reader got right.
  return schema.settings.filter(
    (descriptor) => !values.has(descriptor.id) && applies(descriptor, values),
  ).length
}

/**
 * The container node id the runtime generates for an element type. The prefix
 * is the runtime's, and the SUFFIX is looked up in the catalog it shipped -
 * so this recognises exactly the containers that exist, and stops recognising
 * one the moment the runtime stops generating it.
 *
 * ⚠️ NOT A LIST OF NODE NAMES IN THIS FILE. A hard-coded "archicad.container.wall"
 * here would survive a rename in the runtime and quietly stop matching, leaving
 * a Walls node that draws no contents and says nothing about why.
 */
export const ELEMENT_CONTAINER_PREFIX = 'archicad.container.'

export function containerElementType(nodeType: string, catalog: ElementTypeInfo[]): string {
  if (!nodeType.startsWith(ELEMENT_CONTAINER_PREFIX)) return ''
  const id = nodeType.slice(ELEMENT_CONTAINER_PREFIX.length)
  return catalog.some((type) => type.id === id && type.container) ? id : ''
}

/**
 * The single group a container node holds, read off what it PRODUCED rather
 * than off a stored capture - a container has no capture; its contents are the
 * answer to a question it asked the model on the last run.
 *
 * Empty until the node has run, which is honest: before then the container does
 * not know what it holds, and showing the elements that were wired IN would be
 * showing the unfiltered list under the filtered node's name.
 */
export function containerGroupOf(
  elementType: string,
  output: GraphValue | undefined,
  catalog: ElementTypeInfo[],
): ElementGroup[] {
  const type = catalog.find((candidate) => candidate.id === elementType)
  if (type === undefined) return []
  const guids = (output?.items ?? [])
    .filter((item) => item.valueType === 'archicadElementRef')
    .map((item) => item.text ?? '')
  if (guids.length === 0) return []
  return [{ elementType: type.id, label: type.plural, guids }]
}
