/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @defgroup Protobuf_nano_util Utility functions for nanopb-c
 *
 * These structures are useful when serializing and deserializing either vectors
 * of bytes or malloc'd byte streams, using `pb_encode` and `pb_decode`.
 *
 * @see external/nanopb-c
 * @{
 */

#pragma once

#include <pb.h>

#include <cstdint>
#include <vector>

namespace tuningfork {

/**
 * @brief A view on the vector provided in `vec`. No ownership is taken.
 * @param vec
 *
 * Example usage:
 * \code
 *    std::vector<uint8_t> v;
 *    VectorStream str {&v, 0};
 *    pb_ostream_t stream = {VectorStream::Write, &str, SIZE_MAX, 0};
 *    pb_encode(&stream, ...);
 * \endcode
 */
struct VectorStream {
    /**
     * @brief A vector of bytes that must be valid while `Read` or `Write` are
     * called. The vector will be resized as needed by `Write`.
     */
    std::vector<uint8_t> *vec;
    size_t
        it;  ///< The current position in the vector while decoding or encoding.

    /**
     * Read `count` bytes from the stream to the given buffer.
     * @param stream The stream to read from.
     * @param buf The buffer to write to.
     * @param count The number of bytes to read.
     * @return true if successful, false on a read error.
     */
    static bool Read(pb_istream_t *stream, uint8_t *buf, size_t count);

    /**
     * Write `count` bytes from the given buffer to the stream.
     * @param stream The stream to write to.
     * @param buf The buffer to read from.
     * @param count The number of bytes to write.
     * @return true if successful, false on a write error.
     */
    static bool Write(pb_ostream_t *stream, const uint8_t *buf, size_t count);
};

/**
 * @brief A view on the bytes provided in `vec`. `Write` will call `realloc`
 * if more bytes are needed and it is up to the caller to free the data
 * allocated.
 *
 * It is valid to set `vec=nullptr` and `size=0`, in which case `vec` will be
 * allocated using `malloc`.
 */
struct ByteStream {
    uint8_t *vec;  ///< Pointer to the bytes represented by the stream.
    size_t size;   ///< The size of the bytes that are pointed by `vec`.
    size_t
        it;  ///< The current position in the stream while decoding or encoding.

    /**
     * Read `count` bytes from the stream to the given buffer.
     * @param stream The stream to read from.
     * @param buf The buffer to write to.
     * @param count The number of bytes to read.
     * @return true if successful, false on a read error.
     */
    static bool Read(pb_istream_t *stream, uint8_t *buf, size_t count);

    /**
     * Write `count` bytes from the given buffer to the stream.
     * @param stream The stream to write to.
     * @param buf The buffer to read from.
     * @param count The number of bytes to write.
     * @return true if successful, false on a write error.
     */
    static bool Write(pb_ostream_t *stream, const uint8_t *buf, size_t count);
};

}  // namespace tuningfork

/** @} */
