import { exposed } from '@saucer-dev/types';

export const expectedBridgeProtocolVersion = 1;

export type RuntimeState =
  | 'Uninitialized' | 'Initializing' | 'Ready' | 'Launching'
  | 'Running' | 'Stopping' | 'Error';

export interface AppInfo {
  name: string;
  architecture: string;
  configuration: string;
  development: boolean;
  runtimeState: RuntimeState;
  runtimeIntegrated: boolean;
  bridgeProtocolVersion: number;
}

// Resolve bindings when called so transport failures reach the UI error handler.
export const getAppInfo = () => exposed<AppInfo, []>('getAppInfo')();
export const getRuntimeState = () => exposed<RuntimeState, []>('getRuntimeState')();
export const openDevTools = () => exposed<void, []>('openDevTools')();
