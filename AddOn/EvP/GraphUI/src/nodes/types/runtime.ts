export type ExecutionMode = 'enabled' | 'disabled' | 'bypassed' | 'holding'

export type RuntimeStatus =
  | 'pending'
  | 'running'
  | 'success'
  | 'error'
  | 'blocked'
  | 'disabled'
  | 'bypassed'
  | 'holding'
  | 'cancelled'
