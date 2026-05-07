return {
  name = "core search local or central artifacts",
  request = {
    action = "search",
    system = "maven",
    prompt = "org.example:demo-lib",
  },
  fakeExec = {
    {
      match = "command -v java >/dev/null 2>&1 && command -v mvn >/dev/null 2>&1",
      exitCode = 0,
      stdout = "",
      stderr = "",
      success = true,
    },
    {
      match = "printf '%s' \"${REQPACK_MAVEN_REPO:-}\"",
      exitCode = 0,
      stdout = "/tmp/m2\n",
      stderr = "",
      success = true,
    },
    {
      match = "find '/tmp/m2' -name '*.pom' 2>/dev/null",
      exitCode = 0,
      stdout = "/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom\n",
      stderr = "",
      success = true,
    },
    {
      match = "test -f '/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom' && cat '/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom'",
      exitCode = 0,
      stdout = "<project><packaging>jar</packaging><name>Demo Lib</name><description>Demo artifact</description></project>",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "org.example:demo-lib",
    resultVersion = "1.2.3",
    events = { "searched" },
  }
}
