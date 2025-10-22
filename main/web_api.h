#ifndef NWWEB_H_INCLUDED
#define NWWEB_H_INCLUDED

#define API_HANDLER_ID_GENEGAL                      0x00000000

#define API_CALL_STATUS_COMPLETE                    0x00000000
#define API_CALL_STATUS_EXECUTING                   0x00000001
#define API_CALL_STATUS_CANCELED                    0x00000002
#define API_CALL_STATUS_BUSY                        0x00000003

#define API_CALL_ERROR_STATUS_BAD_REQ               0x80000000
#define API_CALL_ERROR_STATUS_FRAGMENTED            0x80000001
#define API_CALL_ERROR_STATUS_NO_FID                0x80000002
#define API_CALL_ERROR_STATUS_BAD_ARG               0x80000003
#define API_CALL_ERROR_STATUS_NO_FREE_DESCRIPTORS   0x80000004
#define API_CALL_ERROR_STATUS_NO_MEM                0x80000006
#define API_CALL_ERROR_STATUS_NO_ACCESS             0x80000007
#define API_CALL_ERROR_STATUS_NO_HANDLER            0x8000000E
#define API_CALL_ERROR_STATUS_INTERNAL              0x8000000F

/*!
	@brief Web socket API handler proto
	@param[in] pxApiCall API call descriptor
	@param[in/out] ppxContext handler context for current API call, user can link any data to use in future repeated calls 
	@param[in] ulPending pending api calls with in ApiCall session
	@return 0 if prolonged call, else - complete
*/
typedef uint8_t (*ApiHandler_t)(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen);

uint8_t bApiCallRegister(ApiHandler_t fHandler, uint32_t ulFid, void *pxContext);
uint8_t bApiCallUnregister(uint32_t ulFid);

uint8_t bApiCallGetId(void *pxApiCall, uint32_t *ulOutId);
void vApiCallComplete(void *pxApiCall);

uint8_t bApiCallSendStatus(void *pxApiCall, uint32_t ulSta);
uint8_t bApiCallSendJson(void *pxApiCall, const uint8_t *ucJson, uint32_t ulLen);
uint8_t bApiCallSendJsonFidGroup(uint32_t ulFid, const uint8_t *ucData, uint32_t ulLen);

/*
	Snake notation
*/

typedef ApiHandler_t api_handler_t;

uint8_t api_call_register(api_handler_t handler, uint32_t fid, void *context);
uint8_t api_call_unregister(uint32_t fid);

uint8_t api_call_get_id(void *call, uint32_t *out_id);
void api_call_complete(void *call);

uint8_t api_call_send_status(void *call, uint32_t staus);
uint8_t api_call_send_json(void *call, const uint8_t *json, uint32_t len);
uint8_t api_call_send_json_fid_group(uint32_t fid, const uint8_t *json, uint32_t len);

#endif /* NWWEB_H_INCLUDED */ 