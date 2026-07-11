/**
 * @file    nrc.h
 * @brief   UDS 负响应码 (Negative Response Code) 定义，对应 ISO 14229-1。
 */
#ifndef UDS_NRC_H
#define UDS_NRC_H

#define NRC_GENERAL_REJECT                   0x10
#define NRC_SERVICE_NOT_SUPPORTED            0x11
#define NRC_SUB_FUNCTION_NOT_SUPPORTED       0x12
#define NRC_INCORRECT_MESSAGE_LENGTH         0x13
#define NRC_CONDITIONS_NOT_CORRECT           0x22
#define NRC_REQUEST_SEQUENCE_ERROR           0x24
#define NRC_REQUEST_OUT_OF_RANGE             0x31
#define NRC_SECURITY_ACCESS_DENIED           0x33
#define NRC_INVALID_KEY                      0x35
#define NRC_EXCEED_NUMBER_OF_ATTEMPTS        0x36
#define NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED  0x37
#define NRC_SUB_FUNC_NOT_SUPPORTED_IN_SESSION 0x7E
#define NRC_SERVICE_NOT_SUPPORTED_IN_SESSION  0x7F

#endif /* UDS_NRC_H */
