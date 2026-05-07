return {
  name = "core search apt packages",
  request = {
    action = "search",
    system = "sys",
    prompt = "curl",
  },
  fakeExec = {
    {
      match = "printf '%s' \"${REQPACK_SYS_BACKEND:-}\"",
      exitCode = 0,
      stdout = "apt",
      stderr = "",
      success = true,
    },
    {
      match = "printf '%s' \"${REQPACK_SYS_APT_CACHE_BIN:-}\"",
      exitCode = 0,
      stdout = "/usr/bin/apt-cache",
      stderr = "",
      success = true,
    },
    {
      match = "'/usr/bin/apt-cache' search 'curl'",
      exitCode = 0,
      stdout = "curl - Transfer tool\n",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "curl",
    resultVersion = "repo",
    events = { "searched" },
  }
}
