#ifndef _COM_ConvertColourToValueProg_h
#define _COM_ConvertColourToValueProg_h
#include "COM_NodeOperation.h"


/**
  * this program converts an input colour to an output value.
  * it assumes we are in sRGB colour space.
  */
class ConvertColourToValueProg : public NodeOperation {
private:
    /**
      * Cached reference to the inputProgram
      */
	SocketReader * inputOperation;
public:
    /**
      * Default constructor
      */
    ConvertColourToValueProg();

    /**
      * the inner loop of this program
      */
	void executePixel(float* color, float x, float y, MemoryBuffer *inputBuffers[]);

    /**
      * Initialize the execution
      */
    void initExecution();

    /**
      * Deinitialize the execution
      */
    void deinitExecution();
};
#endif
