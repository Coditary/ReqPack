return {
  name = "system fixture info",
  request = {
    action = "info",
    system = "demo-system",
    prompt = "delta",
  },
  fakeExec = {
    {
      match = "demo-pm info delta",
      exitCode = 0,
      stdout = "delta details",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "demo-pm info delta" },
    stdout = { "delta details" },
    events = { "informed" },
    resultCount = 1,
    resultName = "delta",
    resultVersion = "4.5.6",
  }
}
