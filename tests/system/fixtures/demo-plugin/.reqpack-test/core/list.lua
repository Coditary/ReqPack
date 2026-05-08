return {
  name = "system fixture list",
  request = {
    action = "list",
    system = "demo-system",
  },
  fakeExec = {
    {
      match = "demo-pm list",
      exitCode = 0,
      stdout = "alpha entry",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "demo-pm list" },
    stdout = { "alpha entry" },
    events = { "listed" },
    resultCount = 1,
    resultName = "alpha",
    resultVersion = "1.2.3",
  }
}
