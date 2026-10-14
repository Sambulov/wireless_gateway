#ifndef NWWEB_H_INCLUDED
#define NWWEB_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

/* FID — текстовое имя вида "<подсистема>.<действие>": "wifi.sta", "uart2.modbus".
 * Клиент присылает его строкой, сервер возвращает то же имя в ответе. Самое
 * длинное имя сейчас — "uart2.config" (12 символов), 24 хватает с запасом.
 * Отсутствие имени — это пустая строка или NULL, отдельного «нулевого кода»
 * больше нет. */
#define API_FID_NAME_MAX 24

typedef struct webapi_msg {
    /* Имя FID запроса. Копируется в сообщение, а не хранится указателем: имя
     * приходит из кадра, который к моменту разбора очереди уже освобождён. */
    char     fid[API_FID_NAME_MAX];
    int      id;
    uint8_t *data;
    size_t   len;
    /* Socket the request arrived on, -1 for messages a peripheral originates
     * on its own. Lets a handler tell which interface a client is connected
     * through (see request_from_softap() in main/apih_sys.c). */
    int      fd;
} webapi_msg_t;

/* Скопировать имя FID в буфер сообщения. 0 — имя пустое или длиннее предела:
 * такое имя обработчику отдавать нельзя, иначе оно молча обрежется и вызов
 * уйдёт «не тому», кто его ждал. */
static inline uint8_t bApiFidSet(char *dst, const char *name)
{
    if (!dst || !name || !name[0]) return 0;
    size_t n = 0;
    while (name[n] && n < API_FID_NAME_MAX - 1) n++;
    if (name[n] != '\0') return 0;
    for (size_t i = 0; i < n; i++) dst[i] = name[i];
    dst[n] = '\0';
    return 1;
}

#define API_CALL_STATUS_COMPLETE                    0x00000000
#define API_CALL_STATUS_EXECUTING                   0x00000001
#define API_CALL_STATUS_CANCELED                    0x00000002
#define API_CALL_STATUS_BUSY                        0x00000003
#define API_CALL_STATUS_INVALID                     0x00000004
#define API_CALL_STATUS_DELIVERED                   0x00000005


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
	@brief Обработчик вызова API.

	Ядро вызывает его не только на запрос клиента, но и в каждом такте работы,
	поэтому обработчик обязан быть неблокирующим: если нужно ждать периферию,
	он возвращает TRUE и завершает вызов позже, а не крутится внутри.

	@param[in] call      дескриптор вызова. NULL — принудительное завершение:
	                     обработчик обязан освободить свой контекст (*context)
	                     и вернуть FALSE; допускается за несколько вызовов.
	@param[in,out] context контекст вызова, принадлежащий обработчику. При первом
	                     вызове нового запроса содержит контекст, переданный при
	                     регистрации. Пустой *context означает первичный вызов
	                     (например, подписка), заполненный — повторный.
	@param[in] data      для запроса клиента ВСЕГДА не NULL (может указывать на
	                     пустой payload, если ARG не было). data == NULL при живом
	                     вызове — такт работы: обработчик может сделать свою
	                     периодическую работу (например, выдать кадр подписки).
	@param[in] data_len  длина data (0, если ARG не было).
	@return TRUE  — вызов остаётся активным;
	        FALSE — вызов завершён (вызова завершения после этого не будет).
*/
typedef uint8_t (*api_handler_t)(void *call, void **context, uint8_t *data, uint32_t data_len);

typedef api_handler_t ApiHandler_t;

uint8_t bApiCallRegister(api_handler_t fHandler, const char *name, void *pxContext);
uint8_t ws_server_register_fid_queue(const char *name, void *xQueue);
uint8_t bApiCallUnregister(const char *name);

uint8_t bApiCallGetId(void *pxApiCall, uint32_t *ulOutId);
void vApiCallComplete(void *pxApiCall);

uint8_t bApiCallSendStatus(void *pxApiCall, uint32_t ulSta);
uint8_t bApiCallSendJson(void *pxApiCall, const uint8_t *ucJson, uint32_t ulLen);
uint8_t bApiCallSendJsonFidGroup(const char *name, const uint8_t *ucData, uint32_t ulLen);

/*
	Snake notation
*/

uint8_t api_call_register(api_handler_t handler, const char *name, void *context);
uint8_t api_call_unregister(const char *name);

uint8_t api_call_get_id(void *call, uint32_t *out_id);
void api_call_complete(void *call);

uint8_t api_call_send_status(void *call, uint32_t staus);
uint8_t api_call_send_json(void *call, const uint8_t *json, uint32_t len);
uint8_t api_call_send_json_fid_group(const char *name, const uint8_t *json, uint32_t len);

#endif /* NWWEB_H_INCLUDED */ 
