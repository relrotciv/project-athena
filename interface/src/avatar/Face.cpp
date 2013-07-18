//
//  Face.cpp
//  interface
//
//  Created by Andrzej Kapolka on 7/11/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//

#include <glm/gtx/quaternion.hpp>

#include <vpx_decoder.h>
#include <vp8dx.h>

#include <PacketHeaders.h>

#include "Application.h"
#include "Avatar.h"
#include "Head.h"
#include "Face.h"
#include "renderer/ProgramObject.h"

using namespace cv;

ProgramObject* Face::_program = 0;
int Face::_texCoordCornerLocation;
int Face::_texCoordRightLocation;
int Face::_texCoordUpLocation;
GLuint Face::_vboID;
GLuint Face::_iboID;

Face::Face(Head* owningHead) : _owningHead(owningHead), _renderMode(MESH),
        _colorTextureID(0), _depthTextureID(0), _codec(), _frameCount(0) {
    // we may have been created in the network thread, but we live in the main thread
    moveToThread(Application::getInstance()->thread());
}

Face::~Face() {
    if (_codec.name != 0) {
        vpx_codec_destroy(&_codec);
        
        // delete our textures, since we know that we own them
        if (_colorTextureID != 0) {
            glDeleteTextures(1, &_colorTextureID);
        }
        if (_depthTextureID != 0) {
            glDeleteTextures(1, &_depthTextureID);
        }
    }
}

int Face::processVideoMessage(unsigned char* packetData, size_t dataBytes) {
    if (_codec.name == 0) {
        // initialize decoder context
        vpx_codec_dec_init(&_codec, vpx_codec_vp8_dx(), 0, 0);
    }
    // skip the header
    unsigned char* packetPosition = packetData;

    int frameCount = *(uint32_t*)packetPosition;
    packetPosition += sizeof(uint32_t);
    
    int frameSize = *(uint32_t*)packetPosition;
    packetPosition += sizeof(uint32_t);
    
    int frameOffset = *(uint32_t*)packetPosition;
    packetPosition += sizeof(uint32_t);
    
    if (frameCount < _frameCount) { // old frame; ignore
        return dataBytes; 
    
    } else if (frameCount > _frameCount) { // new frame; reset
        _frameCount = frameCount;
        _frameBytesRemaining = frameSize;
        _arrivingFrame.resize(frameSize);
    }
    
    int payloadSize = dataBytes - (packetPosition - packetData);
    memcpy(_arrivingFrame.data() + frameOffset, packetPosition, payloadSize);
    
    if ((_frameBytesRemaining -= payloadSize) <= 0) {
        int result = vpx_codec_decode(&_codec, (const uint8_t*)_arrivingFrame.constData(), _arrivingFrame.size(), 0, 0);
        vpx_codec_iter_t iterator = 0;
        vpx_image_t* image;
        while ((image = vpx_codec_get_frame(&_codec, &iterator)) != 0) {
            // convert from YV12 to RGB
            Mat frame(image->h, image->w, CV_8UC3);
            uchar* ysrc = image->planes[0];
            uchar* vsrc = image->planes[1];
            uchar* usrc = image->planes[2];
            const int RED_V_WEIGHT = (int)(1.403 * 256);
            const int GREEN_V_WEIGHT = (int)(0.714 * 256);
            const int GREEN_U_WEIGHT = (int)(0.344 * 256);
            const int BLUE_U_WEIGHT = (int)(1.773 * 256);
            for (int i = 0; i < image->h; i += 2) {
                for (int j = 0; j < image->w; j += 2) {
                    uchar* tl = frame.ptr(i, j);
                    uchar* tr = frame.ptr(i, j + 1);
                    uchar* bl = frame.ptr(i + 1, j);
                    uchar* br = frame.ptr(i + 1, j + 1);
                    
                    int v = *vsrc++ - 128;
                    int u = *usrc++ - 128;
                    
                    int redOffset = (RED_V_WEIGHT * v) >> 8;
                    int greenOffset = (GREEN_V_WEIGHT * v + GREEN_U_WEIGHT * u) >> 8;
                    int blueOffset = (BLUE_U_WEIGHT * u) >> 8;
                    
                    int ytl = ysrc[0];
                    int ytr = ysrc[1];
                    int ybl = ysrc[image->w];
                    int ybr = ysrc[image->w + 1];
                    ysrc += 2;
                    
                    tl[0] = ytl + redOffset;
                    tl[1] = ytl - greenOffset;
                    tl[2] = ytl + blueOffset;
                    
                    tr[0] = ytr + redOffset;
                    tr[1] = ytr - greenOffset; 
                    tr[2] = ytr + blueOffset;
                    
                    bl[0] = ybl + redOffset;
                    bl[1] = ybl - greenOffset;
                    bl[2] = ybl + blueOffset;
                    
                    br[0] = ybr + redOffset;
                    br[1] = ybr - greenOffset;
                    br[2] = ybr + blueOffset;
                }
                ysrc += image->w;
            }
            QMetaObject::invokeMethod(this, "setFrame", Q_ARG(cv::Mat, frame));
        }
    }
    
    return dataBytes;
}

bool Face::render(float alpha) {
    if (_colorTextureID == 0 || _textureRect.size.area() == 0) {
        return false;
    }
    glPushMatrix();
    
    glTranslatef(_owningHead->getPosition().x, _owningHead->getPosition().y, _owningHead->getPosition().z);
    glm::quat orientation = _owningHead->getOrientation();
    glm::vec3 axis = glm::axis(orientation);
    glRotatef(glm::angle(orientation), axis.x, axis.y, axis.z);
    float scale = BODY_BALL_RADIUS_HEAD_BASE * _owningHead->getScale();
    glScalef(scale, scale, scale);

    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    
    Point2f points[4];
    _textureRect.points(points);
    
    float aspect = _textureRect.size.height / _textureRect.size.width;
    
    if (_depthTextureID != 0) {
        const int VERTEX_WIDTH = 100;
        const int VERTEX_HEIGHT = 100;
        const int VERTEX_COUNT = VERTEX_WIDTH * VERTEX_HEIGHT;
        const int ELEMENTS_PER_VERTEX = 2;
        const int BUFFER_ELEMENTS = VERTEX_COUNT * ELEMENTS_PER_VERTEX;
        const int QUAD_WIDTH = VERTEX_WIDTH - 1;
        const int QUAD_HEIGHT = VERTEX_HEIGHT - 1;
        const int QUAD_COUNT = QUAD_WIDTH * QUAD_HEIGHT;
        const int TRIANGLES_PER_QUAD = 2;   
        const int INDICES_PER_TRIANGLE = 3;
        const int INDEX_COUNT = QUAD_COUNT * TRIANGLES_PER_QUAD * INDICES_PER_TRIANGLE;
        
        if (_program == 0) {
            _program = new ProgramObject();
            _program->addShaderFromSourceFile(QGLShader::Vertex, "resources/shaders/face.vert");
            _program->addShaderFromSourceFile(QGLShader::Fragment, "resources/shaders/face.frag");
            _program->link();
            
            _program->bind();
            _program->setUniformValue("depthTexture", 0);
            _program->setUniformValue("colorTexture", 1);
            _program->release();
            
            _texCoordCornerLocation = _program->uniformLocation("texCoordCorner");
            _texCoordRightLocation = _program->uniformLocation("texCoordRight");
            _texCoordUpLocation = _program->uniformLocation("texCoordUp");
            
            glGenBuffers(1, &_vboID);
            glBindBuffer(GL_ARRAY_BUFFER, _vboID);
            float* vertices = new float[BUFFER_ELEMENTS];
            float* vertexPosition = vertices;
            for (int i = 0; i < VERTEX_HEIGHT; i++) {
                for (int j = 0; j < VERTEX_WIDTH; j++) {
                    *vertexPosition++ = j / (float)(VERTEX_WIDTH - 1);
                    *vertexPosition++ = i / (float)(VERTEX_HEIGHT - 1);
                }
            }
            glBufferData(GL_ARRAY_BUFFER, BUFFER_ELEMENTS * sizeof(float), vertices, GL_STATIC_DRAW);
            delete[] vertices;
            
            glGenBuffers(1, &_iboID);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _iboID);
            int* indices = new int[INDEX_COUNT];
            int* indexPosition = indices;
            for (int i = 0; i < QUAD_HEIGHT; i++) {
                for (int j = 0; j < QUAD_WIDTH; j++) {
                    *indexPosition++ = i * VERTEX_WIDTH + j;
                    *indexPosition++ = (i + 1) * VERTEX_WIDTH + j;
                    *indexPosition++ = i * VERTEX_WIDTH + j + 1;
                    
                    *indexPosition++ = i * VERTEX_WIDTH + j + 1;
                    *indexPosition++ = (i + 1) * VERTEX_WIDTH + j;
                    *indexPosition++ = (i + 1) * VERTEX_WIDTH + j + 1;
                }
            }
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_COUNT * sizeof(int), indices, GL_STATIC_DRAW);
            delete[] indices;
            
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, _vboID);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _iboID);
        }
        glBindTexture(GL_TEXTURE_2D, _depthTextureID);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, _colorTextureID);
        
        _program->bind();
        _program->setUniformValue(_texCoordCornerLocation, 
            points[0].x / _textureSize.width, points[0].y / _textureSize.height);
        _program->setUniformValue(_texCoordRightLocation,
            (points[3].x - points[0].x) / _textureSize.width, (points[3].y - points[0].y) / _textureSize.height);
        _program->setUniformValue(_texCoordUpLocation,
            (points[1].x - points[0].x) / _textureSize.width, (points[1].y - points[0].y) / _textureSize.height);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, 0);
        
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_EQUAL, 1.0f);
        
        if (_renderMode == MESH) {
            glDrawRangeElementsEXT(GL_TRIANGLES, 0, VERTEX_COUNT - 1, INDEX_COUNT, GL_UNSIGNED_INT, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        
        } else { // _renderMode == POINTS
            glPointSize(5.0f);
            glDrawArrays(GL_POINTS, 0, VERTEX_COUNT);
            glPointSize(1.0f);
        }
        
        glDisable(GL_ALPHA_TEST);
        
        glDisableClientState(GL_VERTEX_ARRAY);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        _program->release();
        
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    
    } else {
        glBindTexture(GL_TEXTURE_2D, _colorTextureID);
        glEnable(GL_TEXTURE_2D);
        
        glBegin(GL_QUADS);
        glTexCoord2f(points[0].x / _textureSize.width, points[0].y / _textureSize.height);
        glVertex3f(0.5f, -aspect * 0.5f, -0.5f);    
        glTexCoord2f(points[1].x / _textureSize.width, points[1].y / _textureSize.height);
        glVertex3f(0.5f, aspect * 0.5f, -0.5f);
        glTexCoord2f(points[2].x / _textureSize.width, points[2].y / _textureSize.height);
        glVertex3f(-0.5f, aspect * 0.5f, -0.5f);
        glTexCoord2f(points[3].x / _textureSize.width, points[3].y / _textureSize.height);
        glVertex3f(-0.5f, -aspect * 0.5f, -0.5f);
        glEnd();
        
        glDisable(GL_TEXTURE_2D);
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    glPopMatrix();
    
    return true;
}

void Face::cycleRenderMode() {
    _renderMode = (RenderMode)((_renderMode + 1) % RENDER_MODE_COUNT);    
}

void Face::setFrame(const cv::Mat& color) {
    if (_colorTextureID == 0) {
        glGenTextures(1, &_colorTextureID);
        glBindTexture(GL_TEXTURE_2D, _colorTextureID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, color.cols, color.rows, 0, GL_RGB, GL_UNSIGNED_BYTE, color.ptr());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        _textureSize = color.size();
        _textureRect = RotatedRect(Point2f(color.cols * 0.5f, color.rows * 0.5f), _textureSize, 0.0f);
        
    } else {
        glBindTexture(GL_TEXTURE_2D, _colorTextureID);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, color.cols, color.rows, GL_RGB, GL_UNSIGNED_BYTE, color.ptr());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

