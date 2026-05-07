return {
  name = "core list apt packages",
  request = {
    action = "list",
    system = "sys",
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
      match = "printf '%s' \"${REQPACK_SYS_DPKG_QUERY_BIN:-}\"",
      exitCode = 0,
      stdout = "/usr/bin/dpkg-query",
      stderr = "",
      success = true,
    },
    {
      match = "test -x '/usr/bin/dpkg-query'",
      exitCode = 0,
      stdout = "",
      stderr = "",
      success = true,
    },
    {
      match = "'/usr/bin/dpkg-query' -W -f='${Package}\t${Version}\n'",
      exitCode = 0,
      stdout = "curl\t8.0\n",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "curl",
    resultVersion = "8.0",
    events = { "listed" },
  }
}
