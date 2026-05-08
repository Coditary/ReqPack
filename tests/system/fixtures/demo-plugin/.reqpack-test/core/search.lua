return {
  name = "system fixture search",
  request = {
    action = "search",
    system = "demo-system",
    prompt = "delta",
  },
  fakeExec = {
    {
      match = "demo-pm search delta",
      exitCode = 0,
      stdout = "delta summary",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "demo-pm search delta" },
    stdout = { "delta summary" },
    events = { "searched" },
    resultCount = 1,
    resultName = "delta",
    resultVersion = "9.9.9",
  }
}
