export interface LogEntry {
  id: number
  at: string
  level: 'info' | 'warn' | 'error'
  source: string
  message: string
}

export type LogWriter = (level: LogEntry['level'], source: string, message: string) => void
