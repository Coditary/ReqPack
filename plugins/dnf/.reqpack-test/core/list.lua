return {
  name = "core list installed packages",
  request = {
    action = "list",
    system = "dnf",
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
      match = "dnf repoquery --installed --qf '%{name}.%{arch}\\t%{version}-%{release}\\n'",
      exitCode = 0,
      stdout = "curl.x86_64\t8.0-1\n",
      stderr = "",
      success = true,
    },
    {
      match = "dnf repoquery --installed --qf '%{name}.%{arch}\\t%{summary}\\n'",
      exitCode = 0,
      stdout = "curl.x86_64\tTransfer tool\n",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    commands = {
      "dnf repoquery --installed --qf '%{name}.%{arch}\\t%{version}-%{release}\\n'",
      "dnf repoquery --installed --qf '%{name}.%{arch}\\t%{summary}\\n'",
    },
    stdout = {
      "curl.x86_64\t8.0-1\n",
      "curl.x86_64\tTransfer tool\n",
    },
    resultCount = 1,
    resultName = "curl",
    resultVersion = "8.0-1",
    events = { "listed" },
  }
}
