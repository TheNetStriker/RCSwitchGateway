Import ("env")

#print(env.Dump())

lines = open("ota.txt","r").read().splitlines()

port = lines[0]
password = lines[1]

env.Append(CPPDEFINES=[("OTA_PASSWORD", "\\\"" + password + "\\\"")])

if env["PIOENV"] == "release":
  env.Replace(
      UPLOAD_PROTOCOL="espota",
      UPLOAD_PORT=port,
      UPLOAD_FLAGS=["--port=8266", "--auth=" + password],
  )