#ifndef _COM_ConvertColorToBWOperation_h
#define _COM_ConvertColorToBWOperation_h
#include "COM_NodeOperation.h"


/**
  * this program converts an input colour to an output value.
  * it assumes we are in sRGB colour space.
  */
class ConvertColorToBWOperation : public NodeOperation {
private:
    /**
      * Cached reference to the inputProgram
      */
	SocketReader * inputOperation;
public:
    /**
      * Default constructor
      */
    ConvertColorToBWOperation();

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
