# API

### Api call structure
Format: JSON\
Property `FID`(function ID) - decimal or hexadecimal numder of API function\
Property `ARG` - payload, json object\
Property `SID`(session ID, responce only) - auto incrementing request counter for private subscriptions, tick counter for public subscriptions.

### Echo test (1000)
Request `{"FID":1000, "ARG":{"key":"value"}}`\
Response `{"FID":"0x000003e8","SID":"0x000001eb","ARG":{"key":"value"}}`

### Private subscription or long task test (1001)
Request `{"FID":1001}`\
Response `{"FID":"0x000003e9","SID":"0x00000223","ARG":{"STA":"0x00000000"}}`\
Now user receiving personal data asinchroniusly\
Secondary request `{"FID":1001}`\
Response `{"FID":"0x000003e9","SID":"0x00000223","ARG":{"STA":"0x00000001"}}`\
Task and subscription cancelled

### Public subscription test (1002)
Request `{"FID":1002}`\
Response(personal) `{"FID":"0x000003ea","SID":"0x00000224","ARG":{"STA":"0x00000000"}}`\
Task notifies all subscribers with the same data asinchroniusly\
Response(public) `{"FID":"0x000003ea","SID":"0x0010e13b","ARG":{"data":"Async test"}}`\
Response(public) `{"FID":"0x000003ea","SID":"0x0010e525","ARG":{"data":"Async test"}}`\
...\
Secondary request `{"FID":1002}`\
Response(personal) `{"FID":"0x000003e9","SID":"0x00000224","ARG":{"STA":"0x00000001"}}`\
User subscription cancelled


### Modbus request (2000)
FN {byte} - function number\
ADR {byte} - modbus device address\
RA(optional) {word} - register address\
RVA(optional) {word} - register value or amount\
CV(optional) {byte} - code value\
RD(optoanal) {depends on function} - registers value(s)\
// RAW(unsupported) {any bytes seq <=255 bytes len} - transfer raw data (see uart api)\
AWT(optional) {dword} - awaite responce timeout ms (0: dotn't awaite, [default] >0: 100ms min)\
RDL(optional) {dword} - auto repeat delay ms ([default]0: dotn't repeat, >0:(100ms min))\
TIDC - cancel modbus task with TID. If set, other options ignoreg
#### Example:
Request: `{"FID":2000,"ARG":{"AWT":500,"RDL":500,"FN":3,"ADR":"0x01","RA":0,"RVC":20}}`\
Response: `{"FID":"0x000007d0","SID":"0x00000006","ARG":{"TID":"0x0000000A","ADR":"0x01","FN":"0x03","CV":"0x00","RA":"0x0000","RC":"0x14","RD":["0x04d2","0x223d","0x0000","0x1165","0x0000","0x0022","0x0002","0x1d0d","0x0059","0x0162","0x18d2","0x0000","0x0022","0x0044","0x0000","0x0381","0x7eb3","0x0000","0x0024","0x0003"]}}`\
Request: `{"FID":2000,"ARG":{"TIDC":10}}`\
Response: `{"FID":"0x000007d0","SID":"0x00000006","ARG":{"STA":"0x00000002"}`
