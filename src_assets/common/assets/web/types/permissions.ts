/**
 * Apollo Client Permission System
 * Based on the permission enum from Apollo's crypto.h
 */

export enum Permission {
  // Input permission group
  INPUT_CONTROLLER = 0x00000100,
  INPUT_TOUCH = 0x00000200,
  INPUT_PEN = 0x00000400,
  INPUT_MOUSE = 0x00000800,
  INPUT_KBD = 0x00001000,
  ALL_INPUTS = 0x00001f00,

  // Operation permission group
  CLIPBOARD_SET = 0x00010000,
  CLIPBOARD_READ = 0x00020000,
  FILE_UPLOAD = 0x00040000,
  FILE_DOWNLOAD = 0x00080000,
  SERVER_CMD = 0x00100000,
  ALL_OPERATIONS = 0x001f0000,

  // Action permission group
  LIST = 0x01000000,
  VIEW = 0x02000000,
  LAUNCH = 0x04000000,
  ALLOW_VIEW = 0x06000000,
  ALL_ACTIONS = 0x07000000,

  // Special permissions
  DEFAULT = 0x03000000,
  NONE = 0x00000000,
  ALL = 0x071f1f00,
}

export const PERMISSION_MAPPING: Record<string, number> = {
  input_controller: Permission.INPUT_CONTROLLER,
  input_touch: Permission.INPUT_TOUCH,
  input_pen: Permission.INPUT_PEN,
  input_mouse: Permission.INPUT_MOUSE,
  input_kbd: Permission.INPUT_KBD,
  clipboard_set: Permission.CLIPBOARD_SET,
  clipboard_read: Permission.CLIPBOARD_READ,
  file_upload: Permission.FILE_UPLOAD,
  file_dwnload: Permission.FILE_DOWNLOAD,
  server_cmd: Permission.SERVER_CMD,
  list: Permission.LIST,
  view: Permission.VIEW,
  launch: Permission.LAUNCH,
};

export interface PermissionDefinition {
  name: string;
  suppressed_by: string[];
}

export interface PermissionGroup {
  name: string;
  permissions: PermissionDefinition[];
}

export const PERMISSION_GROUPS: PermissionGroup[] = [
  {
    name: 'Action',
    permissions: [
      { name: 'list', suppressed_by: ['view', 'launch'] },
      { name: 'view', suppressed_by: ['launch'] },
      { name: 'launch', suppressed_by: [] },
    ],
  },
  {
    name: 'Operation',
    permissions: [
      { name: 'server_cmd', suppressed_by: [] },
      { name: 'clipboard_set', suppressed_by: [] },
      { name: 'clipboard_read', suppressed_by: [] },
      { name: 'file_upload', suppressed_by: [] },
      { name: 'file_dwnload', suppressed_by: [] },
    ],
  },
  {
    name: 'Input',
    permissions: [
      { name: 'input_controller', suppressed_by: [] },
      { name: 'input_touch', suppressed_by: [] },
      { name: 'input_pen', suppressed_by: [] },
      { name: 'input_mouse', suppressed_by: [] },
      { name: 'input_kbd', suppressed_by: [] },
    ],
  },
];

export interface ClientPermissions {
  uuid: string;
  name: string;
  perm: number;
  connected?: boolean;
}

/**
 * Check if a specific permission is granted
 */
export function hasPermission(perm: number, permission: string): boolean {
  const permValue = PERMISSION_MAPPING[permission];
  return permValue !== undefined && (perm & permValue) !== 0;
}

/**
 * Check if a permission is suppressed by a higher-level permission
 */
export function isPermissionSuppressed(
  perm: number,
  permission: string,
  suppressedBy: string[],
): boolean {
  return suppressedBy.some((suppressor) => hasPermission(perm, suppressor));
}

/**
 * Toggle a specific permission bit
 */
export function togglePermission(perm: number, permission: string): number {
  const permValue = PERMISSION_MAPPING[permission];
  return permValue !== undefined ? perm ^ permValue : perm;
}

/**
 * Format permission as hex string (e.g., 03
