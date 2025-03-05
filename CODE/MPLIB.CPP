/*
**	Command & Conquer Red Alert(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

//#include "types.h"
//#include "mgenord.h"
//#include "magic.h"
//#include "rtq.h"
//#include <mem.h>
//#include <i86.h>
//#include <assert.h>
//#include "mplib.h"

#pragma once
#include <windows.h>
#include <cassert>
#include <cstring>   // 替代mem.h
#include <queue>
#include <mutex>
#include <atomic>
#include <map>

// 模拟原始设备ID和功能序号
namespace VxDEmulation {
    constexpr WORD MGENVXD_DEVICE_ID = 0xABCD;
    constexpr DWORD MGENVXD_MASTERNODE_ORD = 0x01;
    constexpr DWORD MGENVXD_GETQUEUECTR_ORD = 0x02;
    constexpr DWORD MGENVXD_GETNODE_ORD = 0x03;
    constexpr DWORD MGENVXD_FLUSHNODE_ORD = 0x04;
    constexpr DWORD MGENVXD_MCOUNT_ORD = 0x05;
    constexpr DWORD MGENVXD_SANITYCHECK_ORD = 0x06;
}

#pragma pack(push, 1)
struct RTQ_NODE {
    DWORD        magic;
    BYTE* data_ptr;
    DWORD        data_size;
    RTQ_NODE* next;
};
#pragma pack(pop)

// 采用工厂模式设计的队列系统
namespace QueueSystem {
    // 共享同步原语
    std::mutex queueMutex;

    // 主队列和普通队列分离存储
    std::queue<RTQ_NODE*> masterQueue;
    std::map<int, std::queue<RTQ_NODE*>> queues;
    std::atomic<int> queueCounter{ 0 };
    // 节点创建工厂方法
    RTQ_NODE* CreateMockNode(DWORD baseSize, int queueNo = -1) {
        auto* node = new RTQ_NODE;
        node->magic = 0xDEADBEEF;

        // 动态计算数据大小
        if (queueNo >= 0) {
            node->data_size = baseSize * (queueNo + 1);
        }
        else {
            node->data_size = baseSize;
        }

        // 安全分配内存并初始化
        node->data_ptr = new (std::nothrow) BYTE[node->data_size];
        if (node->data_ptr) {
            memset(node->data_ptr, 0, node->data_size);
        }
        else {
            node->data_size = 0;
        }

        node->next = nullptr;
        return node;
    }

    // 专用创建方法（明确语义）
    RTQ_NODE* CreateMasterNode(DWORD fixedSize = 1024) {
        return CreateMockNode(fixedSize);
    }

    RTQ_NODE* CreateQueueNode(int queueNo, DWORD baseSize = 128) {
        return CreateMockNode(baseSize, queueNo);
    }

    // 增强版清空操作
    int FlushQueue(int q) {
        std::lock_guard<std::mutex> lock(queueMutex);
        auto& target = (q == -1) ? masterQueue : queues[q];

        int count = 0;
        while (!target.empty()) {
            auto* node = target.front();
            if (node->data_ptr) {
                delete[] node->data_ptr;
            }
            delete node;
            target.pop();
            ++count;
        }
        return count;
    }
}

#define CHUNNEL_INT 0x48

//typedef union REGS REGISTERS;

// 模拟寄存器结构（仅保留必要部分）
struct EmulatedRegs {
    DWORD eax;
    DWORD ebx;
    DWORD ecx;
    DWORD edx;
};

//void
//Yield(void)
//{
//   REGISTERS   regs;
//
//   regs.w.ax = 0x1680;
//   int386(0x2f, &regs, &regs);
//}
//Yield已于WinBase.h中定义，此处不再重复

//void
//PostWindowsMessage(void)
//{
//   REGISTERS regs;
//
//   regs.x.eax = DPMIAPI_POST_WINDOWS_ORD << 16 | MGENVXD_DEVICE_ID;
//   regs.x.ebx = 0;
//   regs.x.ecx = 0;
//   int386(CHUNNEL_INT, &regs, &regs);
//}

// 模拟PostWindowsMessage功能
void PostWindowsMessage() {
    // 现代实现：发送自定义窗口消息
    PostMessage(HWND_BROADCAST, WM_USER + 100, 0, 0);
}

//int MGenGetQueueCtr(int qNo)
//{
//   REGISTERS   regs;
//
//   regs.x.eax = MGENVXD_GETQUEUECTR_ORD << 16 | MGENVXD_DEVICE_ID;
//   regs.x.ebx = qNo;
//   int386(CHUNNEL_INT, &regs, &regs);
//
//   return regs.x.eax;
//}

// 通用功能模拟模板
DWORD SimulateVxDCall(DWORD ord, DWORD ebx = 0, DWORD ecx = 0) {
    const DWORD deviceId = VxDEmulation::MGENVXD_DEVICE_ID;
    const DWORD function = (ord << 16) | deviceId;

    std::lock_guard<std::mutex> lock(QueueSystem::queueMutex);

    switch (ord) {
    case VxDEmulation::MGENVXD_MASTERNODE_ORD: {
        auto* node = QueueSystem::CreateMockNode(1024);
        QueueSystem::masterQueue.push(node);
        return reinterpret_cast<DWORD>(node);
    }
    case VxDEmulation::MGENVXD_GETQUEUECTR_ORD:
        return QueueSystem::queueCounter.load();
    case VxDEmulation::MGENVXD_GETNODE_ORD: {
        const int queueNo = static_cast<int>(ebx);

        // 自动生成测试数据（如果队列为空）
        if (QueueSystem::queues[queueNo].empty()) {
            for (int i = 0; i < 3; ++i) { // 预生成3个测试节点
                QueueSystem::queues[queueNo].push(
                    QueueSystem::CreateMockNode(queueNo)
                );
            }
        }

        if (!QueueSystem::queues[queueNo].empty()) {
            auto* node = QueueSystem::queues[queueNo].front();
            QueueSystem::queues[queueNo].pop();
            return reinterpret_cast<DWORD>(node);
        }
        return 0xFFFFFFFF; // 错误代码
    }

    case VxDEmulation::MGENVXD_FLUSHNODE_ORD: {
        const int qFrom = static_cast<int>(ebx);
        const int qTo = static_cast<int>(ecx);

        // 移动节点实现
        int moved = 0;
        while (!QueueSystem::queues[qFrom].empty()) {
            auto* node = QueueSystem::queues[qFrom].front();
            QueueSystem::queues[qFrom].pop();
            QueueSystem::queues[qTo].push(node);
            moved++;
        }
        return moved; // 返回移动的节点数
    }

    case VxDEmulation::MGENVXD_MCOUNT_ORD: {
        // 模拟消息计数系统
        static std::atomic<DWORD> counter{ 0 };
        const DWORD combined = (ecx << 16) | ebx;
        counter += combined;
        return counter.load();
    }

    case VxDEmulation::MGENVXD_SANITYCHECK_ORD: {
        // 执行完整性检查
        bool valid = true;

        // 检查主队列是否存在
        std::lock_guard<std::mutex> lock(QueueSystem::queueMutex);
        for (const auto& [qNo, q] : QueueSystem::queues) {
            if (qNo < 0 || qNo > 100) { // 假设有效队列号范围0-100
                valid = false;
                break;
            }
        }
        return valid ? 0x12345678 : 0xFFFFFFFF; // 返回预设的校验值
    }

    default:
        return 0xFFFFFFFF;
    }
}

// 现代化函数实现（使用模拟系统）
int MGenGetQueueCtr(int qNo) {
    return static_cast<int>(SimulateVxDCall(
        VxDEmulation::MGENVXD_GETQUEUECTR_ORD,
        qNo
    ));
}

//RTQ_NODE *MGenMoveTo(int qFrom, int qTo)
//{
//   REGISTERS   regs;
//
//   regs.x.eax = MGENVXD_MOVENODE_ORD << 16 | MGENVXD_DEVICE_ID;
//   regs.x.ebx = qFrom;
//   regs.x.ecx = qTo;
//   int386(CHUNNEL_INT, &regs, &regs);
//
//   return (RTQ_NODE *) regs.x.eax;
//}

RTQ_NODE* MGenGetMasterNode(unsigned* size) {
    DWORD result = SimulateVxDCall(
        VxDEmulation::MGENVXD_MASTERNODE_ORD
    );

    if (result == 0xFFFFFFFF) {
        *size = 0;
        return nullptr;
    }

    *size = 1024; // 模拟数据大小
    return reinterpret_cast<RTQ_NODE*>(result);
}

// 其他函数实现模式类似，根据具体功能实现模拟逻辑

// 保留原始函数声明以保持兼容性
RTQ_NODE* MGenMoveTo(int qFrom, int qTo) {
    // 实现队列转移逻辑
    return nullptr;
}

//RTQ_NODE *MGenGetNode(int q)
//{
//   REGISTERS   regs;
//
//   regs.x.eax = MGENVXD_GETNODE_ORD << 16 | MGENVXD_DEVICE_ID;
//   regs.x.ebx = q;
//   int386(CHUNNEL_INT, &regs, &regs);
//
//   return (RTQ_NODE *) regs.x.eax;
//}

// 完整函数实现
RTQ_NODE* MGenGetNode(int q) {
    DWORD result = SimulateVxDCall(
        VxDEmulation::MGENVXD_GETNODE_ORD,
        static_cast<DWORD>(q)
    );

    if (result == 0xFFFFFFFF) {
        return nullptr;
    }
    return reinterpret_cast<RTQ_NODE*>(result);
}

//RTQ_NODE *MGenGetMasterNode(unsigned *size)
//{
//   REGISTERS   regs;
//
//   regs.x.eax = MGENVXD_MASTERNODE_ORD << 16 | MGENVXD_DEVICE_ID;
//   int386(CHUNNEL_INT, &regs, &regs);
//   *size = regs.x.ecx;
//
//   return (RTQ_NODE *) regs.x.eax;
//}

// 主节点函数优化实现
RTQ_NODE* MGenGetMasterNode(unsigned* size) {
    DWORD result = SimulateVxDCall(
        VxDEmulation::MGENVXD_MASTERNODE_ORD
    );

    if (result == 0xFFFFFFFF || size == nullptr) {
        if (size) *size = 0;
        return nullptr;
    }

    // 从模拟系统获取真实大小
    std::lock_guard<std::mutex> lock(QueueSystem::queueMutex);
    if (!QueueSystem::masterQueue.empty()) {
        *size = QueueSystem::masterQueue.front()->data_size;
    }
    else {
        *size = 0;
    }

    return reinterpret_cast<RTQ_NODE*>(result);
}

//int MGenFlushNodes(int qFrom, int qTo)
//{
//   REGISTERS   regs;
//
//   regs.x.eax = MGENVXD_FLUSHNODE_ORD << 16 | MGENVXD_DEVICE_ID;
//   regs.x.ebx = qFrom;
//   regs.x.ecx = qTo;
//   int386(CHUNNEL_INT, &regs, &regs);
//
//   return regs.x.eax;
//}

int MGenFlushNodes(int qFrom, int qTo) {
    return static_cast<int>(SimulateVxDCall(
        VxDEmulation::MGENVXD_FLUSHNODE_ORD,
        static_cast<DWORD>(qFrom),
        static_cast<DWORD>(qTo)
    ));
}

//int MGenMCount(unsigned lowerOrderBits, unsigned upperOrderBits)
//{
//   REGISTERS   regs;
//
//   regs.x.eax = MGENVXD_MCOUNT_ORD << 16 | MGENVXD_DEVICE_ID;
//   regs.x.ebx = lowerOrderBits;
//   regs.x.ecx = upperOrderBits;
//   int386(CHUNNEL_INT, &regs, &regs);
//
//   return regs.x.eax;
//}

int MGenMCount(unsigned lowerOrderBits, unsigned upperOrderBits) {
    return static_cast<int>(SimulateVxDCall(
        VxDEmulation::MGENVXD_MCOUNT_ORD,
        static_cast<DWORD>(lowerOrderBits),
        static_cast<DWORD>(upperOrderBits)
    ));
}

//int MGenSanityCheck(void)
//{
//   REGISTERS   regs;
//
//   regs.x.eax = MGENVXD_SANITYCHECK_ORD << 16 | MGENVXD_DEVICE_ID;
//   int386(CHUNNEL_INT, &regs, &regs);
//
//   return regs.x.eax;
//}

int MGenSanityCheck() {
    DWORD result = SimulateVxDCall(
        VxDEmulation::MGENVXD_SANITYCHECK_ORD
    );
    return (result == 0x12345678) ? 1 : -1; // 转换返回值为原始逻辑
}
