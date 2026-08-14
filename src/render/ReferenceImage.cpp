#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "render/ReferenceImage.h"
#include <iostream>

GLuint loadTextureFromFile(const std::string& path, int* outWidth, int* outHeight) {
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) {
        std::cerr << "Failed to load reference image: " << path 
                  << " | Reason: " << stbi_failure_reason() << std::endl;
        if (outWidth) *outWidth = 0;
        if (outHeight) *outHeight = 0;
        return 0;
    }

    if (outWidth) *outWidth = width;
    if (outHeight) *outHeight = height;

    GLuint textureId = 0;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

    std::cout << "Successfully loaded reference image: " << path 
              << " (" << width << "x" << height << ")" << std::endl;
    return textureId;
}

GLuint loadTextureFromMemory(const uint8_t* buffer, size_t size, int* outWidth, int* outHeight) {
    if (!buffer || size == 0) return 0;
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load_from_memory(buffer, static_cast<int>(size), &width, &height, &channels, 4);
    if (!data) {
        std::cerr << "Failed to load reference image from memory | Reason: " << stbi_failure_reason() << std::endl;
        if (outWidth) *outWidth = 0;
        if (outHeight) *outHeight = 0;
        return 0;
    }

    if (outWidth) *outWidth = width;
    if (outHeight) *outHeight = height;

    GLuint textureId = 0;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

    std::cout << "Successfully loaded reference image from memory (" << width << "x" << height << ")" << std::endl;
    return textureId;
}

