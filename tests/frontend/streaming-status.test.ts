import { mount } from '@vue/test-utils';
import { afterEach, describe, expect, it, vi } from 'vitest';
import StreamingStatus from '@/components/StreamingStatus.vue';
import { useAuthStore } from '@/stores/auth';
import { useSessionsStore } from '@/stores/sessions';

vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn(),
}));

vi.mock('@/stores/sessions', () => ({
  useSessionsStore: vi.fn(),
}));

function deferred(): { promise: Promise<void>; resolve: () => void } {
  let resolve!: () => void;
  const promise = new Promise<void>((res) => {
    resolve = res;
  });
  return { promise, resolve };
}

async function flushPromises(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
}

describe('StreamingStatus', () => {
  const mockedUseAuthStore = vi.mocked(useAuthStore);
  const mockedUseSessionsStore = vi.mocked(useSessionsStore);

  afterEach(() => {
    vi.clearAllMocks();
  });

  it('does not start polling after unmount when auth resolves late', async () => {
    const authReady = deferred();
    const startPolling = vi.fn();
    const stopPolling = vi.fn();

    mockedUseAuthStore.mockReturnValue({
      waitForAuthentication: () => authReady.promise,
    } as any);
    mockedUseSessionsStore.mockReturnValue({
      isStreaming: false,
      startPolling,
      stopPolling,
    } as any);

    const wrapper = mount(StreamingStatus as any);
    wrapper.unmount();

    authReady.resolve();
    await flushPromises();

    expect(startPolling).not.toHaveBeenCalled();
    expect(stopPolling).toHaveBeenCalledTimes(1);
  });
});
