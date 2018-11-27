/*******************************************************************************
 * Copyright (c) 2017-2018 Marc Jakobi, github.com/MrcJkb, fortiss GmbH
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *    Marc Jakobi - initial implementation for HTTP clients
 *    Jose Cabral - Merge old HTTPIpLayer to this one and use CIEC_STRING
 ********************************************************************************/

#include "httplayer.h"
#include "httpparser.h"
#include "../../arch/devlog.h"
#include <string.h>
#include "basecommfb.h"
#include "http_handler.h"
#include "comtypes.h"

using namespace forte::com_infra;

CHttpComLayer::CHttpComLayer(CComLayer* paUpperLayer, CBaseCommFB* paComFB) :
    CComLayer(paUpperLayer, paComFB), mInterruptResp(e_Nothing), mRequestType(e_NOTSET), mPort(80), mBufFillSize(0), hasOutputResponse(false), mCorrectlyInitialized(false), mHasParameter(false){
  memset(mRecvBuffer, 0, sizeof(mRecvBuffer));
}

CHttpComLayer::~CHttpComLayer(){
  closeConnection();
}

EComResponse CHttpComLayer::openConnection(char *paLayerParameter){
  EComResponse eRetVal = e_InitInvalidId;
  //Check that all connected data types of RDs and SDs are strings. See: https://bugs.eclipse.org/bugs/show_bug.cgi?id=538391

  switch (m_poFb->getComServiceType()){
    case e_Server:
      if(1 == m_poFb->getNumSD()){
        mPath = paLayerParameter;
        getExtEvHandler<CHTTP_Handler>().addServerPath(this, mPath);
        eRetVal = e_InitOk;
      }
      else{
        DEVLOG_ERROR("[HTTP Layer] The FB with PARAM %s coudln't be initialized since only one SD is possible in HTTP Server which is for the response\n", paLayerParameter);
      }
      break;
    case e_Client:
      eRetVal = openClientConnection(paLayerParameter);
      break;
    case e_Publisher:
    case e_Subscriber:
      // HTTP does not use UDP
      eRetVal = e_ProcessDataInvalidObject;
      break;
  }
  mCorrectlyInitialized = (eRetVal == e_InitOk);
  return eRetVal;
}

EComResponse CHttpComLayer::openClientConnection(char* paLayerParameter){
  EComResponse eRetVal = e_InitInvalidId;
  unsigned int numberOfSD = m_poFb->getNumSD();
  unsigned int numberOfRD = m_poFb->getNumRD();
  bool error = false;
  bool defaultResponse = true;
  bool defaultContent = true;
  char* helperChar = strchr(paLayerParameter, ';'); //look for type of request
  if(0 != helperChar){
    *helperChar = '\0';
    helperChar++;
    char* startOfContentType = strchr(helperChar, ';'); //look for content type
    if(0 != startOfContentType){
      *startOfContentType++ = '\0';
      char* startOfResponse = strchr(startOfContentType, ';');
      if(0 != startOfResponse){
        *startOfResponse++ = '\0';
        mExpectedRspCode = startOfResponse;
        defaultResponse = false;
      }
      mContentType = startOfContentType;
      defaultContent = false;
    }
    if(0 == strcmp(helperChar, "POST")){
      mRequestType = e_POST;
    }else if(0 == strcmp(helperChar, "PUT")){
      mRequestType = e_PUT;
    }else if(0 == strcmp(helperChar, "GET")){
      mRequestType = e_GET;
    }else{
      error = true;
      DEVLOG_ERROR("[HTTP Layer] GET, PUT or POST must be defined, but %s was defined instead\n", helperChar);
    }
  }else{
    error = true;
    DEVLOG_ERROR("[HTTP Layer] GET, PUT or POST must be defined, but none of them was defined\n");
  }

  if(!error){
    if(defaultResponse){
      mExpectedRspCode = "HTTP/1.1 200 OK";
    }
    if(defaultContent){
      mContentType = "text/html";
    }

    helperChar = strchr(paLayerParameter, '?'); //look for parameters in path
    if(0 != helperChar && (e_PUT == mRequestType || e_POST == mRequestType)){
      *helperChar++ = '\0';
      mReqData = helperChar;
      mHasParameter = true;
      mContentType = "application/x-www-form-urlencoded";
    }

    helperChar = strchr(paLayerParameter, '/'); //look for path
    if(0 != helperChar){
      *helperChar++ = '\0';
      mPath = helperChar;
      helperChar = strchr(paLayerParameter, ':'); //look for port, if not found, it was already set to 80 in the constructor
      if(0 != helperChar){
        *helperChar++ = '\0';
        mPort = static_cast<TForteUInt16>(forte::core::util::strtoul(helperChar, 0, 10));
      }else{
        DEVLOG_INFO("[HTTP Layer] No port was found on the parameter, using default 80\n");
      }
      mHost = paLayerParameter;

      switch (mRequestType){
        case e_PUT:
        case e_POST: {
          if("" != mReqData && 0 != numberOfSD){
            DEVLOG_WARNING("[HTTP Layer] Parameters in PARAMS are used for PUT/POST request data instead of the SDs\n");
          }else if("" == mReqData && 1 != numberOfSD){
            DEVLOG_ERROR("[HTTP Layer] You are using a POST/PUT FB but no data is defined as SD nor as parameters in PARAMS\n");
            break;
          }

          if(1 < numberOfRD){
            DEVLOG_ERROR("[HTTP Layer] A PUT/POST request with more than one output\n");
            break;
          }else{
            if(1 == numberOfRD){ //can have 0 RD, so response is ignored
              hasOutputResponse = true;
            }
            CHttpParser::createPutPostRequest(mRequest, mHost, mPath, mReqData, mContentType, mRequestType);
            eRetVal = e_InitOk;
            DEVLOG_INFO("[HTTP Layer] FB with PUT/POST request initialized. Host: %s, Path: %s\n", mHost.getValue(), mPath.getValue());
          }
          break;
        }
        case e_GET: {
          if(1 == numberOfRD){
            CHttpParser::createGetRequest(mRequest, mHost, mPath);
            eRetVal = e_InitOk;
            hasOutputResponse = true;
            DEVLOG_INFO("[HTTP Layer] FB with GET request initialized. Host: %s, Path: %s\n", mHost.getValue(), mPath.getValue());
          }else if(0 == numberOfRD){
            DEVLOG_ERROR("[HTTP Layer] A GET request without and output doesn't make sense\n");
          }
          else{
            DEVLOG_ERROR("[HTTP Layer] A GET request with more than one output\n");
          }
          break;
        }
        default:
          break;
      }
    }
    else{
      DEVLOG_ERROR("[HTTP Layer] No path was found on the parameter\n");
    }
  }else{
    DEVLOG_ERROR("[HTTP Layer] Wrong PARAM value\n");
  }

  m_eConnectionState = e_Disconnected;
  return eRetVal;
}

EComResponse CHttpComLayer::sendData(void *paData, unsigned int){
  mInterruptResp = e_Nothing;
  if(mCorrectlyInitialized){
    switch (m_poFb->getComServiceType()){
      case e_Server:
        sendDataAsServer(paData);
        break;
      case e_Client:
        sendDataAsClient(paData);
        break;
      case e_Publisher:
      case e_Subscriber:
        // HTTP does not use UDP
        break;
    }
  }else{
    DEVLOG_ERROR("[HTTP Layer]The FB is not initialized\n");
  }

  return mInterruptResp;
}

void CHttpComLayer::sendDataAsServer(const void *paData){
  bool error = false;
  TConstIEC_ANYPtr apoSDs = static_cast<TConstIEC_ANYPtr>(paData);
  if(!serializeData(apoSDs[0])){
    error = true;
  }
  else{
    if(!CHttpParser::createResponse(mRequest, "200 OK", mContentType, mReqData)){
      DEVLOG_DEBUG("[HTTP Layer] Wrong Response request when changing the data\n");
      error = true;
    }
  }
  if(error){
    getExtEvHandler<CHTTP_Handler>().forceClose(this);
    mInterruptResp = e_ProcessDataDataTypeError;
  }
  else{
    getExtEvHandler<CHTTP_Handler>().sendServerAnswer(this, mRequest);
    mInterruptResp = e_ProcessDataOk;
  }
}

void CHttpComLayer::sendDataAsClient(const void *paData){
  bool error = false;
  if(!mHasParameter && (e_PUT == mRequestType || e_POST == mRequestType)){
    TConstIEC_ANYPtr apoSDs = static_cast<TConstIEC_ANYPtr>(paData);
    if(!serializeData(apoSDs[0])){
      error = true;
      DEVLOG_ERROR("[HTTP Layer] Error in data serialization\n");
    }else{
      if(!CHttpParser::changePutPostData(mRequest, mReqData)){
        DEVLOG_ERROR("[HTTP Layer] Wrong PUT/POST request when changing the data\n");
        error = true;
      }
    }
  }

  if(!error){
    if(getExtEvHandler<CHTTP_Handler>().sendClientData(this, mRequest)){
      mInterruptResp = e_ProcessDataOk;
    }else{
      mInterruptResp = e_ProcessDataSendFailed;
      DEVLOG_ERROR("[HTTP Layer] Sending request on TCP failed\n");
    }
  }else{
    mInterruptResp = e_ProcessDataDataTypeError;
  }
}

EComResponse CHttpComLayer::recvData(const void *paData, unsigned int paSize){
  mInterruptResp = e_Nothing;
  if(mCorrectlyInitialized){
    memcpy(mRecvBuffer, paData, (paSize > cg_unIPLayerRecvBufferSize) ? cg_unIPLayerRecvBufferSize : paSize);
    switch (m_poFb->getComServiceType()){
      case e_Server:
        DEVLOG_ERROR("[HTTP Layer] Receiving raw data as a Server? That's wrong, use the recvServerData function\n");
        break;
      case e_Client:
        if(0 == paData){ //timeout occurred
          mInterruptResp = e_ProcessDataRecvFaild;
        }else{
          if(e_ProcessDataOk != (mInterruptResp = handleHTTPResponse(mRecvBuffer))){
            DEVLOG_ERROR("[HTTP Layer] FB with host: %s:%u couldn't handle the HTTP response\n", mHost.getValue(), mPort);
          }else{
            //TODO Trigger event?
          }
        }
        break;
      default:
        break;
    }
  }else{
    DEVLOG_ERROR("[HTTP Layer]The FB is not initialized\n");
  }
  if(e_ProcessDataOk == mInterruptResp){
    m_poFb->interruptCommFB(this);
  }
  return mInterruptResp;
}

EComResponse forte::com_infra::CHttpComLayer::recvServerData(CSinglyLinkedList<CIEC_STRING>& paParameterNames, CSinglyLinkedList<CIEC_STRING>& paParameterValues){
  mInterruptResp = e_Nothing;
  bool failed = false;
  if(0 < m_poFb->getNumSD()){
    unsigned int noOfParameters = 0;
    for(CSinglyLinkedList<CIEC_STRING>::Iterator iter = paParameterNames.begin(); iter != paParameterNames.end(); ++iter){
      noOfParameters++;
    }

    if(noOfParameters == m_poFb->getNumRD()){
      noOfParameters = 0;
      for(CSinglyLinkedList<CIEC_STRING>::Iterator iter = paParameterValues.begin(); iter != paParameterValues.end(); ++iter){
        m_poFb->getRDs()[noOfParameters].setValue(*iter);
      }
      //TODO: How do we handle the names? For now the parameters are put in the same order they arrived
    }else{
      DEVLOG_ERROR("[HTTP Layer] FB with path %s received a number of parameters of %u, while it has %u SDs\n", mPath.getValue(), static_cast<TForteUInt16>(noOfParameters), m_poFb->getNumSD());
      failed = true;
    }
  }

  if(failed){
    CIEC_STRING toSend;
    CIEC_STRING result = "400 Bad Request";
    mReqData = "";
    if(!CHttpParser::createResponse(toSend, result, mContentType, mReqData)){
      DEVLOG_DEBUG("[HTTP Layer] Wrong Response request when changing the data\n");
      getExtEvHandler<CHTTP_Handler>().forceCloseFromRecv(this);
    }
    else{
      getExtEvHandler<CHTTP_Handler>().sendServerAnswerFromRecv(this, toSend);
    }
    mInterruptResp = e_ProcessDataDataTypeError;
  }
  else{
    mInterruptResp = e_ProcessDataOk;
  }
  if(e_ProcessDataOk == mInterruptResp){
    m_poFb->interruptCommFB(this);
  }
  return mInterruptResp;
}

EComResponse CHttpComLayer::handleHTTPResponse(char *paData){
  DEVLOG_DEBUG("[HTTP Layer] Handling received HTTP response\n");
  EComResponse eRetVal = e_ProcessDataRecvFaild;
  if(0 != strstr(paData, "\r\n\r\n")){ // Verify that at least a body has been received
    if(m_poFb != 0){
      CIEC_ANY* apoRDs = m_poFb->getRDs();
      // Interpret HTTP response and set output status according to success/failure.
      CIEC_STRING output;
      bool success = true;
      if(e_GET == mRequestType){
        success = CHttpParser::parseGetResponse(output, paData, mExpectedRspCode);
      }
      else{
        success = CHttpParser::parsePutPostResponse(output, paData, mExpectedRspCode);
      }
      success ? eRetVal = e_ProcessDataOk : eRetVal = e_ProcessDataRecvFaild;
      // Set data output if possible
      if(success && hasOutputResponse){
        apoRDs[0].fromString(output.getValue());
      }
    }
    else{
      DEVLOG_ERROR("[HTTP Layer] No FB defined\n");
    }
  }
  else{
    DEVLOG_ERROR("[HTTP Layer] Invalid or incomplete HTTP response\n");
  }
  return eRetVal;
}

EComResponse CHttpComLayer::processInterrupt(){
  mInterruptResp = e_ProcessDataOk;
  return mInterruptResp;
}

void CHttpComLayer::closeConnection(){
  if (e_Server == m_poFb->getComServiceType()){
      getExtEvHandler<CHTTP_Handler>().removeServerPath(mPath);
  }
  getExtEvHandler<CHTTP_Handler>().forceClose(this);
}

bool CHttpComLayer::serializeData(const CIEC_ANY& paCIECData){
  int bufferSize = paCIECData.getToStringBufferSize();
  char acDataValue[bufferSize];
  int nConsumedBytes;
  switch (paCIECData.getDataTypeID()){
    case CIEC_ANY::e_WSTRING:
      nConsumedBytes = static_cast<const CIEC_WSTRING&>(paCIECData).toUTF8(acDataValue, bufferSize, false);
      break;
    case CIEC_ANY::e_STRING:
      nConsumedBytes = static_cast<const CIEC_STRING&>(paCIECData).toUTF8(acDataValue, bufferSize, false);
      break;
    default:
      nConsumedBytes = paCIECData.toString(acDataValue, bufferSize);
      break;
  }
  if(-1 != nConsumedBytes){
    acDataValue[nConsumedBytes] = '\0';
  }
  mReqData = acDataValue;
  return true;
}

CIEC_STRING& forte::com_infra::CHttpComLayer::getHost(){
  return mHost;
}

TForteUInt16 forte::com_infra::CHttpComLayer::getPort(){
  return mPort;
}
