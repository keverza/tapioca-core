import type { GraphParameter } from '../../types'

/**
 * The camera list a Camera node holds, read off its stored parameter.
 *
 * ⚠️ THE BROWSER PARSES THIS AND DOES NOT AUTHOR IT. Every field here was
 * written by the runtime from what Archicad reported (NodeGraph/ArchicadNodes.cpp
 * and IArchicadHost's ViewCamera), and the only thing the editor does with a
 * camera is DRAW it and name its index when the user presses a button. A camera
 * the browser composed would be a second source of truth for a pose, and the
 * capture that used it would render something nobody had looked at.
 *
 * ⚠️ AND THE FIELD NAMES ARE THE RENDERER'S. They are StartDiligentCapture's
 * own schema, carried unchanged from the capture so no translation sits between
 * the node and the frame it produces. Renaming one here would break nothing
 * visible and everything real.
 */
export interface StoredCamera {
  /** False only for a row a hand-edited document produced; the runtime drops those. */
  valid: boolean
  /** "perspective", or why there was no camera. */
  source: string
  eye: [number, number, number]
  target: [number, number, number]
  viewConeDegreesHorizontal: number
  /**
   * Whether a sun was captured with this camera. False for a row saved before
   * sun capture existed, or one taken in a project with no place information -
   * and it has to be read, because the angles below are then ZERO, which is a
   * real direction (due east on the horizon) rather than a missing one.
   */
  hasSun: boolean
  /**
   * ⚠️ THE MODEL ANGLE, CCW FROM +X - what the renderer takes. It is NOT
   * the compass bearing, which is `sunBearingDegrees` below; the two differ by
   * project north and showing the wrong one to a person is how a correct sun
   * gets reported as a bug.
   */
  sunAzimuthDegrees: number
  sunAltitudeDegrees: number
  /** Clockwise from geographic north - the number a person reads. */
  sunBearingDegrees: number
  /**
   * Which rule produced the angles: "date" when they were computed for the
   * project's moment, "angles" when the user typed them into the Sun dialog.
   *
   * Shown in the row's tooltip because the two ways a sun can be wrong - a
   * frozen one and a discarded typed one - look identical from the numbers.
   */
  sunSource: string
}

function triple(record: Record<string, unknown>, prefix: string): [number, number, number] {
  const axis = (suffix: string): number => {
    const value = record[prefix + suffix]
    return typeof value === 'number' && Number.isFinite(value) ? value : 0
  }
  return [axis('X'), axis('Y'), axis('Z')]
}

/**
 * One stored row, or undefined when it cannot be read.
 *
 * ⚠️ UNDEFINED RATHER THAN A ZEROED CAMERA, for the same reason the runtime's
 * decoder drops such a row: a camera at the origin looking at the origin renders
 * as a plausible entry in the list and points the 3D window nowhere useful the
 * moment somebody presses Restore on it.
 */
export function parseCamera(text: string): StoredCamera | undefined {
  let record: unknown
  try {
    record = JSON.parse(text)
  } catch {
    return undefined
  }
  if (typeof record !== 'object' || record === null) return undefined
  const fields = record as Record<string, unknown>
  if (typeof fields.eyeX !== 'number') return undefined
  const angle = (key: string): number => {
    const value = fields[key]
    return typeof value === 'number' && Number.isFinite(value) ? value : 0
  }
  return {
    valid: fields.valid === true,
    source: typeof fields.source === 'string' ? fields.source : '',
    eye: triple(fields, 'eye'),
    target: triple(fields, 'target'),
    viewConeDegreesHorizontal:
      typeof fields.viewConeDegreesHorizontal === 'number' ? fields.viewConeDegreesHorizontal : 0,
    hasSun: fields.hasSun === true,
    sunAzimuthDegrees: angle('sunAzimuthDegrees'),
    sunAltitudeDegrees: angle('sunAltitudeDegrees'),
    sunBearingDegrees: angle('sunBearingDegrees'),
    sunSource: typeof fields.sunSource === 'string' ? fields.sunSource : '',
  }
}

/**
 * The sun on a row, for a person.
 *
 * ⚠️ THE BEARING, NOT THE MODEL AZIMUTH. A row reading "-80 deg" sends
 * somebody hunting for a bug that is a convention: the renderer wants the model
 * angle and a reader wants the compass one, and this is the reader's end.
 * Undefined when the camera carries no sun, which a caller must draw as absent
 * rather than as a sun on the horizon.
 */
export function formatSun(camera: StoredCamera): string | undefined {
  if (!camera.hasSun) return undefined
  const angles = `${camera.sunBearingDegrees.toFixed(0)}\u00b0 bearing, ${camera.sunAltitudeDegrees.toFixed(0)}\u00b0 up`
  // Named in the tooltip rather than kept internal: "from the date" and
  // "typed in" fail in opposite directions, and somebody looking at a sun
  // they believe is wrong needs to know which of the two they are seeing.
  return camera.sunSource === '' ? angles : `${angles} (${camera.sunSource})`
}

/**
 * The node's cameras, in capture order.
 *
 * ⚠️ THE INDEX A ROW IS DRAWN AT IS THE INDEX THE ACTION SENDS, so an
 * unreadable row must NOT be silently skipped here the way the runtime's decoder
 * skips it - dropping row 2 in the browser while the runtime still holds it
 * would make every Remove below it delete the wrong camera. Unreadable rows are
 * kept as `undefined` and drawn as broken, which keeps every later index true.
 */
export function camerasOf(parameters: GraphParameter[]): (StoredCamera | undefined)[] {
  const stored = parameters.find((parameter) => parameter.parameterId === 'cameras')
  const value = stored?.value
  if (value === undefined) return []
  // A one-item list arrives as the ITEM, not a list of one - the runtime's tree
  // layer projects a single-item branch back as its item, and the native decoder
  // accepts both for exactly this reason. See CamerasFromValue.
  if (value.valueType === 'string') return [parseCamera(value.text ?? '')]
  return (value.items ?? []).map((item) => parseCamera(item.text ?? ''))
}

/** Metres, at the precision a user reads a position in - not a coordinate readout. */
export function formatPosition(position: [number, number, number]): string {
  return position.map((value) => value.toFixed(1)).join(', ')
}
