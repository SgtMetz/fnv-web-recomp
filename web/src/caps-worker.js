// Worker-only capabilities: OPFS sync access handles exist only in dedicated workers.
self.postMessage({
  syncAccess: typeof FileSystemSyncAccessHandle !== 'undefined',
  readOnlyMode: typeof FileSystemSyncAccessHandle !== 'undefined' && 'mode' in FileSystemSyncAccessHandle.prototype,
});
