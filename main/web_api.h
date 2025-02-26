#ifndef NWWEB_H_INCLUDED
#define NWWEB_H_INCLUDED

#include "LinkedList.h"

#define API_HANDLER_ID_GENEGAL                     0x00000000

#define WEB_API_STATUS_OK                          0x00000000
#define WEB_API_STATUS_CANCELED                    0x00000001
#define WEB_API_STATUS_BUSY                        0x00000002
#define WEB_API_STATUS_WR_ERROR                    0x00000003
#define WEB_API_STATUS_ACCESS_ERROR                0x00000004

#define WEB_API_ERROR_STATUS_BAD_REQ               0x80000000
#define WEB_API_ERROR_STATUS_FRAGMENTED            0x80000001
#define WEB_API_ERROR_STATUS_NO_FID                0x80000002
#define WEB_API_ERROR_STATUS_BAD_ARG               0x80000003
#define WEB_API_ERROR_STATUS_NO_FREE_DESCRIPTORS   0x80000004
#define WEB_API_ERROR_STATUS_NO_MEM                0x80000006
#define WEB_API_ERROR_STATUS_NO_HANDLER            0x8000000E
#define WEB_API_ERROR_STATUS_INTERNAL              0x8000000F

/*!
	@brief Web socket API handler proto
	@param[in] ulCallId API call descriptor id
	@param[in/out] ppxInOutCallContext handler context for current API call, user can link any data to use in future repeated call 
	@param[in] pucData request arg (json string)
	@param[in] ulLen arg size
*/
typedef uint8_t (WsRequestHandler_t)(uint32_t ulCallId, void **ppxInOutCallContext, char *pucData, uint32_t ulLen);

typedef void (*ApiDataSerializer_t)(uint8_t *pucBuf, uint32_t *ulInOutSize, void *pxArg);

typedef struct {
  __LinkedListObject__
  WsRequestHandler_t *pfHandler;
  uint32_t ulFunctionId;
} WsApiHandler_t;

uint8_t vWebApiRegisterHandler(WsApiHandler_t *handler);

int32_t lWebApiResponseSerializerFidGroup(uint32_t ulFid, ApiDataSerializer_t fSerializer, void *pxArg);
int32_t lWebApiResponseDataFidGroup(uint32_t ulFid, uint8_t *ucData, uint32_t ulLen);

int32_t lWebApiResponseSerializer(uint32_t ulCallId, ApiDataSerializer_t fSerializer, void *pxArg);
int32_t lWebApiResponseData(uint32_t ulCallId, uint8_t *ucData, uint32_t ulLen);

uint8_t bWebApiResponseStatus(uint32_t ulCallId, uint32_t ulSta);

void vWebApiCallComplete(uint32_t ulCallId);

#endif /* NWWEB_H_INCLUDED */ 