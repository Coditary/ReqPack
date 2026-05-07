return {
  name = "core search packages",
  request = {
    action = "search",
    system = "dnf",
    prompt = "curl",
  },
  fakeExec = {
    {
      match = "command -v dnf >/dev/null 2>&1",
      exitCode = 0,
      stdout = "",
      stderr = "",
      success = true,
    },
    {
      match = "dnf search 'curl' --quiet",
      exitCode = 0,
      stdout = "curl.x86_64\tTransfer tool\n",
      stderr = "",
      success = true,
    },
    {
      match = "dnf repoquery --qf '%{name}.%{arch}\\t%{version}-%{release}\\n' 'curl' 2>/dev/null",
      exitCode = 0,
      stdout = "curl.x86_64\t8.0-1\n",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "curl",
    resultVersion = "8.0-1",
    events = { "searched" },
  }
}
