# coding=utf-8
# This is a sample Python script.

# Press Shift+F10 to execute it or replace it with your code.
# Press Double Shift to search everywhere for classes, files, tool windows, actions, and settings.

import asyncio
import websockets
import pytest

class Register:
    def __init__(self, number, value):
        self.reg = (number, value)

    def printme(self):
        print("register: number:{0}, value:{1}".format(self.reg[0], self.reg[1]))


class Device:
    def __init__(self, name: str, holding_registers: list, addr: int = 1, inputs=[], discrete=[], coils=[]):
        self.name = name
        self.addr = addr
        self.reg_holdings = holding_registers
        self.reg_inputs = inputs
        self.reg_discrete_inputs = discrete
        self.reg_coils = coils

    def getname(self):
        return self.name


class WebApi:
    def __init__(self, fid: int, args: list=[]):
        self.fid = fid
        self.args = args
        self.sid = 0

    def serialize(self):
        req = "{{\"FID\":{0}".format(self.fid)
        if len(self.args) > 0:
            req += ",\"ARG\":"
            for idx, arg in enumerate(self.args):
                req += "{{\"{0}\":{1}}}".format(arg[0], arg[1])
                if idx < len(self.args) - 1:
                    req += ","
        req += "}"
        return req


async def send_single_request(url, req: WebApi):
    req = req.serialize()
    print("Send to {0} request {1}".format(url, req))
    async with websockets.connect(url) as ws:
        try:
            await ws.send(req)
            resp = await ws.recv()
            print("Received from {0} response {1}".format(url, resp))
            return True
        except Exception as e:
            print(e)
            return False 


async def send_request(url, req: WebApi):
    req = req.serialize()
    print("Send to {0} request {1}".format(url, req))
    async with websockets.connect(url) as ws:
        while True:
            try:
                #print('send ping')
                #await ws.ping()
                #print('send data')
                await ws.send(req)
                resp = await ws.recv()
            except Exception as e:
                print(e)
                exit(1)
            print("Received from {0} response {1}".format(url, resp))
            #await asyncio.sleep(5)


class Request:
    def __init__(self, destination: str, request: WebApi):
        self.web_api = request
        self.destination = destination

    # shit arch. fix
    def get_ws(self):
        return self.web_api

    # shit arch. fix
    def get_destination(self):
        return self.destination


class WirelessGateway:
    def __init__(self, url: str):
        self.__url = url

    def url(self):
        return self.__url


class Browser:
    requests = []

    def setup(self, requests: list):
        for req in requests:
            self.requests.append(req)

    async def asyn_test(self):
        while True:
            print("hello")
            await asyncio.sleep(1)

    async def __main_corutine(self):
        tasks = []
        for req in self.requests:
            dst = req.get_destination()
            data = req.get_ws()
            print("url: {}, req: {}".format(dst, data))
            tasks.append(send_request(dst, data))
        await asyncio.gather(*tasks)

    def run(self):
        asyncio.run(self.__main_corutine())



@pytest.mark.skip
#@pytest.mark.asyncio
async def test_ping():
    gw = WirelessGateway("ws://192.168.4.1/ws")
    result = True
    try:
        sock = await websockets.connect(gw.url())
        pong_waiter = await sock.ping()
        latence = await pong_waiter
        print('ping-pong latency: ' + str(latence))
    except:
        print('fault')
        result = False

    await sock.close()
    assert(result)

    
@pytest.mark.skip
#@pytest.mark.asincio
async def test_stress_2000():
    gw = WirelessGateway("ws://192.168.4.1/ws")

    r1 = Request(gw.url(), WebApi(1000))
    r2 = Request(gw.url(), WebApi(1000))
    r3 = Request(gw.url(), WebApi(1000))
    r4 = Request(gw.url(), WebApi(1000))
    r5 = Request(gw.url(), WebApi(1000))

    browser.setup([r1, r2, r3, r4, r5])
    browser.run()


@pytest.mark.skip
#@pytest.mark.asyncio
async def test_1000():
    gw = WirelessGateway("ws://192.168.4.1/ws")
    result = True

    r1 = Request(gw.url(), WebApi(1000))
    try:
        sock = await websockets.connect(gw.url())
        await sock.send(WebApi(1000).serialize())
        resp = await sock.recv()
        sock.close()
        if not "FID" in resp:
            result = False
    except:
        result = False

    assert(result)

@pytest.mark.skip
#@pytest.mark.asyncio
async def test_false_connect():
    gw = WirelessGateway("ws://192.168.4.2/unexsisting_page")
    with pytest.raises(Exception):
        await websockets.connect(gw.url())

@pytest.mark.skip
#@pytest.mark.asyncio
async def test_connect():
    gw = WirelessGateway("ws://192.168.4.1/ws")
    result = True

    try:
        ws = await websockets.connect(gw.url())
        await ws.close()
    except:
        result = False

    assert(result)
