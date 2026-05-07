return {
  name = "core proxy request resolves target",
  request = {
    action = "info",
    system = "java",
    prompt = "demo-artifact",
  },
  fakeExec = {},
  expect = {
    success = true,
    resultCount = 1,
    resultName = "demo-artifact",
    resultVersion = "proxy",
    events = { "informed" },
    eventPayloads = {
      informed = "{description=Java proxy manager, name=demo-artifact, version=proxy}",
    },
  }
}
