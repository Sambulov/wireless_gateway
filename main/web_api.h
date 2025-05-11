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
#define API_CALL_ERROR_STATUS_NO_ACCESS             0x00000007
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

//typedef void (*ApiDataSerializer_t)(uint8_t *pucBuf, uint32_t *ulInOutSize, void *pxArg);

uint8_t bApiCallRegister(ApiHandler_t fHandler, uint32_t ulFid, void *pxContext);
uint8_t bApiCallUnregister(uint32_t ulFid);

//uint8_t bApiCallGetData(void *pxApiCall, uint8_t **ppucData, uint32_t *ulOutDataLen);
uint8_t bApiCallGetId(void *pxApiCall, uint32_t *ulOutId);
uint8_t bApiCallGetSockFd(void *pxApiCall, int *pxOutFd);
void vApiCallComplete(void *pxApiCall);

uint8_t bApiCallSendStatus(void *pxApiCall, uint32_t ulSta);
uint8_t bApiCallSendJson(void *pxApiCall, const uint8_t *ucJson, uint32_t ulLen);
uint8_t bApiCallSendJsonFidGroup(uint32_t ulFid, const uint8_t *ucData, uint32_t ulLen);

//uint8_t bApiCallSendFidGroupSerializer(uint32_t ulFid, ApiDataSerializer_t fSerializer, void *pxArg);
//uint8_t bApiCallSendSerializer(void *pxApiCall, ApiDataSerializer_t fSerializer, void *pxArg);

#endif /* NWWEB_H_INCLUDED */ 