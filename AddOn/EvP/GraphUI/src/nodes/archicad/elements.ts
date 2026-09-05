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

// ---------------------------------------------------------------------------
// THE PROMOTABLE PROPERTY LIST.
//
// ⚠️ THE SCHEMA, NOT THE INSTANCES (§12). What a user browses before promoting
// is "what does a wall have", and that question has ONE answer for four hundred
// walls. Listing every element's every setting would put sixteen hundred rows in
// front of somebody looking for Height, and the ninety-nine per cent of them
// that read the same would carry no information at all.
//
// ⚠️ AND IT IS DRIVEN BY THE RUNTIME'S TABLE. The rows, their order, their
// groups, their units and their types all come from the same response that
// carried the values - so this can never show a property the reader does not
// fill, and never shows one under a label it invented.
// ---------------------------------------------------------------------------

/**
 * The value types whose preview IS a quantity. Everything else previews as a
 * summary of a shape or a name, and a unit after one of those is a category
 * error rather than a formatting slip.
 */
const MEASURED_TYPES = new Set(['double', 'integer', 'point3'])

export interface PropertyRow {
  settingId: string
  label: string
  group: string
  valueType: string
  unit: string
  /** 'archicad' or 'derived'. */
  origin: string
  /**
   * What the elements say, as ONE line: the shared value when they agree, a
   * count when they do not, and empty when none of them answered.
   */
  preview: string
  /**
   * Whether `preview` is one value the elements agree on, rather than a count of
   * how many they disagree by.
   *
   * ⚠️ A COUNT IS NOT A MEASUREMENT, so it must not take the unit. "4 values m"
   * reads as a quantity in metres and is a tally of how many distinct lengths
   * there are.
   */
  sharedValue: boolean
  /**
   * Whether the unit belongs beside this preview.
   *
   * ⚠️ A UNIT BELONGS TO A MEASUREMENT, NOT TO A DESCRIPTION. The reference line
   * carries "m" because its COORDINATES are metres, but its preview is
   * "Polyline (2 points)" - a shape summary - and "Polyline (2 points) m" reads
   * as a length. Same for a count of differing values.
   */
  showUnit: boolean
  /** How many of the elements this build could read this setting for. */
  readCount: number
  /** Applies to the type but was read for none of the elements. */
  unread: boolean
}

export interface PropertyGroup {
  group: string
  rows: PropertyRow[]
}

export interface PropertyList {
  elementType: string
  /** The type's plural, which is what the container is called. */
  label: string
  elementCount: number
  groups: PropertyGroup[]
}

/**
 * The properties of one element type, from the schema and the values that came
 * back with it.
 *
 * ⚠️ A ROW THAT DOES NOT APPLY IS OMITTED, A ROW THAT IS MERELY UNREAD IS KEPT.
 * A Basic wall has no Composite and never will, so offering it for promotion
 * would create a node that answers Absent for every element for ever. A setting
 * that applies and simply was not read is a different thing - it is what this
 * build cannot do YET, it is worth seeing, and it is marked rather than hidden.
 */
export function propertyListOf(
  schema: ElementTypeSchema | undefined,
  elements: readonly ElementDescription[],
): PropertyList | undefined {
  if (schema === undefined) return undefined

  const readable = elements.filter((element) => element.available)
  const valuesOf = readable.map(
    (element) => new Map<string, ElementSettingValue>(element.settings.map((setting) => [setting.id, setting])),
  )

  const groups: PropertyGroup[] = []
  for (const descriptor of schema.settings) {
    // Applicable to at least ONE of the elements in hand. A mixed container of
    // Basic and Composite walls shows both material rows, because each is real
    // for part of the set; a container of only Basic walls shows one.
    if (!valuesOf.some((values) => applies(descriptor, values))) continue

    const texts = valuesOf
      .map((values) => values.get(descriptor.id)?.text)
      .filter((text): text is string => text !== undefined)
    const distinct = new Set(texts)

    let preview = ''
    if (distinct.size === 1) preview = [...distinct][0]
    else if (distinct.size > 1) preview = `${distinct.size} values`

    let section = groups.find((candidate) => candidate.group === descriptor.group)
    if (section === undefined) {
      section = { group: descriptor.group, rows: [] }
      groups.push(section)
    }
    section.rows.push({
      settingId: descriptor.id,
      label: descriptor.label,
      group: descriptor.group,
      valueType: descriptor.valueType,
      unit: descriptor.unit,
      origin: descriptor.origin,
      preview,
      sharedValue: distinct.size === 1,
      showUnit: distinct.size === 1 && descriptor.unit !== '' && MEASURED_TYPES.has(descriptor.valueType),
      readCount: texts.length,
      unread: texts.length === 0,
    })
  }

  return {
    elementType: schema.id,
    label: schema.plural,
    elementCount: elements.length,
    groups,
  }
}

/**
 * The human-readable name of one element.
 *
 * ⚠️ A GUID IS NOT A NAME. It is the identity and it has to stay reachable, but
 * "55614A45-AD1B-4CFD-..." tells a person nothing about which wall they are
 * looking at, and a list of four of them tells them nothing four times. The
 * user's own element ID is what Archicad shows them; the zone name, the library
 * part and the type label are the next best answers, in that order.
 */
export function elementDisplayName(element: ElementDescription): string {
  const setting = (id: string): string => element.settings.find((value) => value.id === id)?.text ?? ''
  return (
    setting('elementId') ||
    setting('zoneName') ||
    setting('libraryPart') ||
    element.typeLabel ||
    element.guid
  )
}
