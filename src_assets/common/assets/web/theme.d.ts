type Theme = 'light' | 'dark' | 'auto';

export declare const getPreferredTheme: () => Theme;
export declare const setStoredTheme: (theme: string) => void;
export declare const setTheme: (theme: Theme) => void;
export declare const showActiveTheme: (theme: Theme, focus?: boolean) => void;
export declare function setupThemeToggleListener(): void;
export declare function loadAutoTheme(): void;
