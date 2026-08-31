/// <reference types="vite/client" />

declare module 'three/addons/controls/OrbitControls.js' {
  export class OrbitControls {
    constructor(camera: unknown, domElement?: HTMLElement)
    enabled: boolean
    enableDamping: boolean
    dampingFactor: number
    screenSpacePanning: boolean
    target: { set(x: number, y: number, z: number): void }
    update(): boolean
    reset(): void
  }
}
