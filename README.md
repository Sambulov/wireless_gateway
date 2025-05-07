# API

### Api call structure
Format: JSON\
Property `FID`(function ID) - decimal or hexadecimal numder of API function\
Property `ARG` - payload json object\
Property `RID`(request ID) - auto incrementing request counter for private subscriptions, tick counter for public subscriptions.

### Echo test (1000)
Request `{"FID":1000, "ARG":{"key":"value"}}`\
Response `{"FID":"0x000003e8","RID":"0x000001eb","ARG":{"key":"value"}}`

### Private subscription or long task test (1001)
Request `{"FID":1001}`\
Response `{"FID":"0x000003e9","RID":"0x00000223","ARG":{"STA":"0x00000000"}}`\
Now user receiving personal data asinchroniusly
Secondary request `{"FID":1001}`\
Response `{"FID":"0x000003e9","RID":"0x00000223","ARG":{"STA":"0x00000001"}}`\
Task and subscription cancelled

### Public subscription test (1002)
Request `{"FID":1002}`\
Response(personal) `{"FID":"0x000003ea","RID":"0x00000224","ARG":{"STA":"0x00000000"}}`\
Task notifies all subscribers with the same data asinchroniusly
Response(public) `{"FID":"0x000003ea","RID":"0x0010e13b","ARG":{"data":"Async test"}}`\
Response(public) `{"FID":"0x000003ea","RID":"0x0010e525","ARG":{"data":"Async test"}}`\
...\
Secondary request `{"FID":1002}`\
Response(personal) `{"FID":"0x000003e9","RID":"0x00000224","ARG":{"STA":"0x00000001"}}`\
User subscription cancelled

