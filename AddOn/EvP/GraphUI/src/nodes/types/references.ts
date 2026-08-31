export interface ArchicadElementRef {
  projectId: string
  guid: string
  elementType?: string
  label?: string
}

export interface ArchicadParameterRef {
  element: ArchicadElementRef
  parameterId: string
}

export interface ArchicadPropertyRef {
  element: ArchicadElementRef
  propertyId: string
}
