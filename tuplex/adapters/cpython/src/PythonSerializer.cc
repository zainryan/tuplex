//--------------------------------------------------------------------------------------------------------------------//
//                                                                                                                    //
//                                      Tuplex: Blazing Fast Python Data Science                                      //
//                                                                                                                    //
//                                                                                                                    //
//  (c) 2017 - 2021, Tuplex team                                                                                      //
//  Created by Leonhard Spiegelberg first on 1/1/2021                                                                 //
//  License: Apache 2.0                                                                                               //
//--------------------------------------------------------------------------------------------------------------------//

#include <Field.h>
#include <TupleTree.h>

#include "PythonSerializer.h"
#include "PythonSerializer_private.h"
#include "PythonHelpers.h"

namespace tuplex {
    namespace cpython {
        PyObject *PyObj_FromCJSONKey(const char *serializedKey) {
            assert(serializedKey);
            assert(strlen(serializedKey) >= 2);
            const char *keyString = serializedKey + 2;
            switch (serializedKey[0]) {
                case 's':
                    return PyUnicode_DecodeUTF8(keyString, strlen(keyString), nullptr);
                case 'b':
                    if (strcmp(keyString, "True") == 0)
                        return PyBool_FromLong(1);
                    if (strcmp(keyString, "False") == 0)
                        return PyBool_FromLong(0);
                    Logger::instance().defaultLogger().error(
                            "invalid boolean key: " + std::string(keyString) + ", returning Py_None");
                    Py_XINCREF(Py_None);
                    return Py_None;
                case 'i':
                    return PyLong_FromString(keyString, nullptr, 10);
                case 'f':
                    return PyFloat_FromDouble(strtod(keyString, nullptr));
                default:
                    Logger::instance().defaultLogger().error("unknown type " + std::string(serializedKey)
                                                             + " in field encountered. Returning Py_None");
                    Py_XINCREF(Py_None);
                    return Py_None;
            }
        }

        PyObject *PyObj_FromCJSONVal(const cJSON *obj, const char type) {
            switch (type) {
                case 's':
                    return PyUnicode_DecodeUTF8(obj->valuestring, strlen(obj->valuestring), nullptr);
                case 'b':
                    return cJSON_IsTrue(obj) ? PyBool_FromLong(1) : PyBool_FromLong(0);
                case 'i':
                    return PyLong_FromDouble(obj->valuedouble);
                case 'f':
                    return PyFloat_FromDouble(obj->valuedouble);
                default:
                    std::string errorString =
                            "unknown type identifier" + std::string(&type) + " in field encountered. Returning Py_None";
                    Logger::instance().defaultLogger().error(errorString);
                    Py_XINCREF(Py_None);
                    return Py_None;
            }
        }

        PyObject *PyDict_FromCJSON(const cJSON *dict) {
            auto dictObj = PyDict_New();
            cJSON *cur_item = dict->child;
            while (cur_item) {
                char *key = cur_item->string;
                auto keyObj = PyObj_FromCJSONKey(key);
                auto valObj = PyObj_FromCJSONVal(cur_item, key[1]);
                PyDict_SetItem(dictObj, keyObj, valObj);
                cur_item = cur_item->next;
            }
            return dictObj;
        }

        PyObject *createPyTupleFromMemory(const uint8_t *ptr, const python::Type &row_type) {
            int64_t current_buffer_index = 0;

            auto tree = tuplex::TupleTree<tuplex::Field>(row_type);
            PyObject *obj = PyTuple_New(row_type.parameters().size());
            std::vector<PyObject *> obj_stack;
            PyObject *curr_obj = obj;

            std::vector<int> curr;
            std::vector<int> prev;

            int bitmap_index = 0;
            const uint8_t *bitmap = ptr;
            auto num_bitmap_fields = core::ceilToMultiple(python::numOptionalFields(row_type), 64ul)/64;
            ptr += sizeof(int64_t) * num_bitmap_fields;

            for (const std::vector<int> &index: tree.getMultiIndices()) {
                curr = index;

                int position_diff = -1;
                if (prev.empty()) {
                    position_diff = 0;
                } else {
                    for (int i = 0; i < std::min(prev.size(), curr.size()); i++) {
                        if (prev[i] != curr[i]) {
                            position_diff = i;
                            break;
                        }
                    }
                }
                if (position_diff != -1) {
                    auto num_elem_to_pop = obj_stack.size() - position_diff;
                    for (int i = 0; i < num_elem_to_pop; i++) {
                        curr_obj = obj_stack.back();
                        obj_stack.pop_back();
                    }
                    std::vector<int> curr_index_stack;
                    for (int i = 0; i < position_diff; i++) {
                        curr_index_stack.push_back(curr[i]);
                    }
                    for (int i = position_diff; i < curr.size() - 1; i++) {
                        int curr_index_value = curr[i];
                        curr_index_stack.push_back(curr_index_value);
                        PyObject *new_tuple = PyTuple_New(tree.fieldType(curr_index_stack).parameters().size());
                        PyTuple_SetItem(curr_obj, curr_index_value, new_tuple);
                        obj_stack.push_back(curr_obj);
                        curr_obj = new_tuple;
                    }
                }

                int curr_index = curr.back();
                python::Type current_type = tree.fieldType(curr);
                PyObject *elem_to_insert = createPyObjectFromMemory(ptr + current_buffer_index, current_type, bitmap, bitmap_index);
                if(current_type.isOptionType()) bitmap_index++;
                if (elem_to_insert == nullptr) {
                    return nullptr;
                }

                PyTuple_SetItem(curr_obj, curr_index, elem_to_insert);
                if (!current_type.withoutOptions().isSingleValued())
                    current_buffer_index += SIZEOF_LONG;

                prev = curr;
            }

            return obj;
        }

        PyObject *createPyDictFromMemory(const uint8_t *ptr) {
#ifndef NDEBUG
            if(!ptr)
                throw std::runtime_error("empty pointer, can't create dict");
#endif

            // access the field element using Tuplex's serialization format.
            auto elem = *(uint64_t *) (ptr);
            auto offset = (uint32_t) elem;
            auto length = (uint32_t) (elem >> 32ul);

            auto cjson_str = reinterpret_cast<const char *>(&ptr[offset]);
            cJSON *dict = cJSON_Parse(cjson_str);
#ifndef NDEBUG
            if(!dict)
                throw std::runtime_error("could not parse string " + std::string(cjson_str));
#endif
            auto test = PyDict_FromCJSON(dict);
            return test;
        }

        PyObject *createPyListFromMemory(const uint8_t *ptr, const python::Type &row_type) {
            assert(row_type.isListType() && row_type != python::Type::EMPTYLIST);
            auto elementType = row_type.elementType();
            if(elementType.isSingleValued()) {
                auto numElements = *(int64_t*)ptr;
                auto ret = PyList_New(numElements);
                for(size_t i=0; i<numElements; i++) {
                    PyObject* element;
                    if(elementType == python::Type::NULLVALUE) {
                        element = Py_None;
                        Py_XINCREF(Py_None);
                    } else if(elementType == python::Type::EMPTYDICT) {
                        element = PyDict_New();
                    } else if(elementType == python::Type::EMPTYTUPLE) {
                        element = PyTuple_New(0);
                    } else if(elementType == python::Type::EMPTYLIST) {
                        element = PyList_New(0);
                    } else {
                        throw std::runtime_error("Invalid list type: " + row_type.desc());
                    }
                    PyList_SET_ITEM(ret, i, element);
                }
                return ret;
            } else {
                // access the field element
                auto elem = *(uint64_t *) ptr;
                auto offset = (uint32_t) elem;
                auto length = (uint32_t) (elem >> 32ul);

                // move to varlen field
                ptr = &ptr[offset];

                // get number of elements
                auto numElements = *(int64_t*)ptr;
                ptr += sizeof(int64_t);
                auto ret = PyList_New(numElements);

                for(size_t i=0; i<numElements; i++) {
                    PyObject* element;
                    if(elementType == python::Type::I64) {
                        element = PyLong_FromLong(*reinterpret_cast<const int64_t*>(ptr));
                    } else if(elementType == python::Type::F64) {
                        element = PyFloat_FromDouble(*reinterpret_cast<const double*>(ptr));
                    } else if(elementType == python::Type::BOOLEAN) {
                        element = PyBool_FromLong(*reinterpret_cast<const int64_t*>(ptr));
                    } else if (elementType == python::Type::STRING) {
                        char *string_errors = nullptr;
                        auto curStrOffset = *reinterpret_cast<const int64_t *>(ptr);
                        int64_t curStrLen;
                        // string lists are serialized (in the varlen section) as | num elements | offset 1 | ... | offset n | string 1 | ... | string n
                        // we need to use the offsets to calculate the lengths of the strings
                        if(i == numElements-1) {
                            // for the final string, we need to calculate the length by subtracting the offset from the total length of the varlen section (minus the spaces used for the first n-1 offsets and the number of elements)
                            curStrLen = (length - (numElements*sizeof(int64_t))) - curStrOffset;
                        } else {
                            // for any string other than the final string, we calculate the length by taking the difference between consecutive offsets (and accounting for the 8 byte shift in where the offsets start from)
                            auto nextStrOffset = *reinterpret_cast<const int64_t*>(ptr+sizeof(int64_t));
                            curStrLen = nextStrOffset - (curStrOffset-sizeof(int64_t));
                        }
                        element = PyUnicode_DecodeUTF8(reinterpret_cast<const char*>(&ptr[curStrOffset]), curStrLen-1, string_errors);
                    } else if(elementType.isTupleType()) {
                        element = createPyTupleFromMemory(ptr, elementType);
                    } else if(elementType.isDictionaryType()) {
                        element = createPyDictFromMemory(ptr);
                    } else throw std::runtime_error("Invalid list type: " + row_type.desc());
                    PyList_SET_ITEM(ret, i, element);
                    ptr += sizeof(int64_t);
                }
                return ret;
            }
        }

        PyObject *createPyObjectFromMemory(const uint8_t *ptr, const python::Type &row_type, const uint8_t *bitmap, int index) {
            if (row_type == python::Type::BOOLEAN) {
                return PyBool_FromLong(ptr[0]);
            } else if (row_type == python::Type::I64) {
                return PyLong_FromLong(*(int64_t *) (ptr));
            } else if (row_type == python::Type::F64) {
                return PyFloat_FromDouble(*(double *) (ptr));
            } else if (row_type == python::Type::STRING) {
                auto elem = *(uint64_t *) (ptr);
                auto offset = (uint32_t) elem;
                auto length = (uint32_t) (elem >> 32ul);
                auto str = reinterpret_cast<const char *>(&ptr[offset]);
                char *string_errors = nullptr;
                return PyUnicode_DecodeUTF8(str, length - 1, string_errors);
            } else if (row_type == python::Type::EMPTYTUPLE) {
                return PyTuple_New(0);
            } else if (row_type.isTupleType()) {
                return createPyTupleFromMemory(ptr, row_type);
            } else if (row_type == python::Type::EMPTYDICT) {
                return PyDict_New();
            } else if (row_type.isDictionaryType() || row_type == python::Type::GENERICDICT) {
                return createPyDictFromMemory(ptr);
            } else if(row_type == python::Type::EMPTYLIST) {
                return PyList_New(0);
            } else if(row_type.isListType()) {
                return createPyListFromMemory(ptr, row_type);
            } else if(row_type.isOptionType()) { // TODO: should this be [isOptional()]?
                if(!bitmap) {
                    // If bitmap was null, this means that it was a single value, not part of a tuple
                    bitmap = ptr;
                    index = 0;
                    ptr += (sizeof(uint64_t)/sizeof(uint8_t));
                }
                bool is_null = bitmap[index/64] & (1UL << (index % 64));
                if(is_null) {
                    Py_XINCREF(Py_None);
                    return Py_None;
                }

                auto t = row_type.getReturnType();
                return createPyObjectFromMemory(ptr, t);
            } else if(row_type == python::Type::PYOBJECT) {
                // cloudpickle, deserialize
                auto elem = *(uint64_t *) (ptr);
                auto offset = (uint32_t) elem;
                auto buf_size = (uint32_t) (elem >> 32ul);
                auto buf = reinterpret_cast<const char *>(&ptr[offset]);
                return python::deserializePickledObject(python::getMainModule(), buf, buf_size);
            } else {
#ifndef NDEBUG
                Logger::instance().logger("serializer").debug("unknown type '" + row_type.desc() + "' encountered, replacing with None.");
#endif
            }
            Py_XINCREF(Py_None);
            return Py_None;
        }

        int64_t checkTupleCapacity(const uint8_t *ptr, int64_t capacity, const python::Type &row_type) {
            auto tree = tuplex::TupleTree<tuplex::Field>(row_type);
            auto indices = tree.getMultiIndices();
            auto num_bytes = static_cast<int64_t>(indices.size() * sizeof(int64_t));
            Logger::instance().logger("python").debug("checkTupleCapacity num_bytes: " + std::to_string(num_bytes));
            if (num_bytes > capacity) {
                return -1;
            }

            // check that varlen field doesn't overflow the capacity
            auto num_bitmap_fields = core::ceilToMultiple(python::numOptionalFields(row_type), 64ul)/64;
            auto varlen_field_length = *((int64_t*)(ptr + sizeof(int64_t) * num_bitmap_fields + num_bytes));
            Logger::instance().logger("python").debug("checkTupleCapacity num_bitmap_fields: " + std::to_string(num_bitmap_fields));
            Logger::instance().logger("python").debug("checkTupleCapacity varlen_field_length: " + std::to_string(varlen_field_length));
            if(sizeof(int64_t) * num_bitmap_fields + num_bytes + varlen_field_length > capacity) {
                return -1;
            }

            return num_bytes;
        }

        int64_t serializationSize(const uint8_t *ptr, int64_t capacity, const python::Type &row_type) {

            // should be identical to Deserializer.inferlength...

            // handle special empty cases
            if (row_type.isSingleValued())
                return 0;

            auto num_bitmap_fields = core::ceilToMultiple(python::numOptionalFields(row_type), 64ul) / 64;
            // option[()], option[{}] only have bitmap
            if (row_type.isOptionType() && row_type.getReturnType().isSingleValued())
                return sizeof(int64_t) * num_bitmap_fields;

            // move ptr by bitmap
            ptr += sizeof(int64_t) * num_bitmap_fields;

            // decode rest of fields...
            int64_t capacitySize = sizeof(int64_t);
            if ((row_type == python::Type::STRING || row_type.isDictionaryType() ||
                    row_type == python::Type::GENERICDICT ||
                    (row_type.isListType() && row_type != python::Type::EMPTYLIST && !row_type.elementType().isSingleValued())) &&
                row_type != python::Type::EMPTYDICT) {
                auto elem = *(uint64_t *) (ptr);
                auto offset = (uint32_t) elem;
                auto length = (uint32_t) (elem >> 32ul);
                if (offset + length > capacity) {
                    capacitySize = -1;
                }
            } else if (row_type != python::Type::EMPTYTUPLE && row_type.isTupleType()) {
                capacitySize = checkTupleCapacity(ptr, capacity, row_type);
            }

            if (capacitySize <= -1) {
                return capacitySize;
            }
            capacitySize += sizeof(int64_t) * num_bitmap_fields;

            if (!row_type.isFixedSizeType()) {
                int64_t var_length_region_length = *(int64_t *) (ptr + capacitySize);
                capacitySize += var_length_region_length + sizeof(int64_t);
            }

            return capacitySize;
        }

        bool isCapacityValid(const uint8_t *ptr, int64_t capacity, const python::Type &row_type) {
            if (capacity <= 0) {
                return false;
            }

            int64_t size = serializationSize(ptr, capacity, row_type);

            if (size <= -1) {
                return false;
            }

            return size <= capacity;
        }


        // TODO: check for errors when creating PyObjects
        bool fromSerializedMemory(const uint8_t *ptr, int64_t capacity, const tuplex::Schema &schema, PyObject **obj,
                                  const uint8_t **nextptr) {
            python::Type row_type = schema.getRowType();


            // @TODO: fix this function in debug mode
            // this function is not working. leave for now...
            // check is anyways a waste of time...
            // // check for bitmap size & push pointer...
            // if (!isCapacityValid(ptr, capacity, row_type)) {
            //     return false;
            // }

            *obj = createPyObjectFromMemory(ptr, row_type);

            if (nextptr) {
                *nextptr = ptr + serializationSize(ptr, capacity, row_type);
            }
            return *obj != nullptr;
        }
    }
}