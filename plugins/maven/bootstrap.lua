function bootstrap()
    return reqpack.exec.run("command -v java >/dev/null 2>&1 && command -v mvn >/dev/null 2>&1").success
end
