# coding=utf-8
# This is a sample Python script.

# Press Shift+F10 to execute it or replace it with your code.
# Press Double Shift to search everywhere for classes, files, tool windows, actions, and settings.

import asyncio
import websockets


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


async def send_request(url, req: WebApi):
    req = req.serialize()
    print("Send to {0} request {1}".format(url, req))
    async with websockets.connect(url) as ws:
        while True:
            await ws.send(req)
            resp = await ws.recv()
            print("Received from {0} response {1}".format(url, resp))


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


if __name__ == '__main__':
    gw = WirelessGateway("ws://192.168.4.1/ws")
    browser = Browser()

    #r1 = Request(gw.url(), WebApi(2000, [("AWT", "500"), ("RDL", 500)]))
    #r2 = Request(gw.url(), WebApi(2000, [("AWT", "600"), ("RDL", 600)]))
    #r3 = Request(gw.url(), WebApi(2000, [("AWT", "700"), ("RDL", 700)]))
    #r4 = Request(gw.url(), WebApi(2000, [("AWT", "800"), ("RDL", 800)]))
    #r5 = Request(gw.url(), WebApi(2000, [("AWT", "900"), ("RDL", 900)]))

    r1 = Request(gw.url(), WebApi(1010))
    r2 = Request(gw.url(), WebApi(1010))
    r3 = Request(gw.url(), WebApi(1010))
    r4 = Request(gw.url(), WebApi(1010))
    r5 = Request(gw.url(), WebApi(1010))

    browser.setup([r1, r2, r3, r4, r5])

    browser.run()
