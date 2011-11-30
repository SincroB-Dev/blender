#include "COM_DirectionalBlurOperation.h"
#include "COM_InputSocket.h"
#include "COM_OutputSocket.h"
#include "BLI_math.h"

extern "C" {
	#include "RE_pipeline.h"
}

DirectionalBlurOperation::DirectionalBlurOperation() : NodeOperation() {
	this->addInputSocket(*(new InputSocket(COM_DT_COLOR)));
	this->addOutputSocket(*(new OutputSocket(COM_DT_COLOR)));
	this->setComplex(true);

	this->inputProgram = NULL;
}

void* DirectionalBlurOperation::initializeTileData(rcti *rect, MemoryBuffer **memoryBuffers) {
	void* buffer = inputProgram->initializeTileData(NULL, memoryBuffers);
	return buffer;
}

void DirectionalBlurOperation::initExecution() {
	this->inputProgram = getInputSocketReader(0);
	QualityStepHelper::initExecution(COM_QH_INCREASE);
	const float angle = this->data->angle;
	const float zoom = this->data->zoom;
	const float spin = this->data->spin;
	const float iterations = this->data->iter;
	const float distance = this->data->distance;
	const float center_x = this->data->center_x;
	const float center_y = this->data->center_y;
	const float width = getWidth();
	const float height = getHeight();

	const float a= angle * (float)M_PI / 180.f;
	const float itsc= 1.f / pow(2.f, (float)iterations);
	float D;

	D= distance * sqrtf(width*width + height*height);
	center_x_pix= center_x * width;
	center_y_pix= center_y * height;

	tx=  itsc * D * cos(a);
	ty= -itsc * D * sin(a);
	sc=  itsc * zoom;
	rot= itsc * spin * (float)M_PI / 180.f;

}

void DirectionalBlurOperation::executePixel(float* color, int x, int y, MemoryBuffer *inputBuffers[], void* data) {
	const int iterations = this->data->iter*this->data->iter;
	float col[4]= {0,0,0,0};
	float col2[4]= {0,0,0,0};
	color[0] = 0;
	color[1] = 0;
	color[2] = 0;
	color[3] = 0;
	float ltx = tx;
	float lty = ty;
	float lsc = sc;
	float lrot = rot;
	/* blur the image */
	for(int i= 0; i < iterations; ++i) {
		const float cs= cos(lrot), ss= sin(lrot);
		const float isc= 1.f / (1.f + lsc);

		const float v= isc * (y - center_y_pix) + lty;
		const float u= isc * (x - center_x_pix) + ltx;

		this->inputProgram->read(col, cs * u + ss * v + center_x_pix, cs * v - ss * u + center_y_pix, inputBuffers);

		col2[0] += col[0];
		col2[1] += col[1];
		col2[2] += col[2];
		col2[3] += col[3];

		/* double transformations */
		ltx += tx;
		lty += ty;
		lrot += rot;
		lsc += sc;
	}
	color[0] = col2[0]/iterations;
	color[1] = col2[1]/iterations;
	color[2] = col2[2]/iterations;
	color[3] = col2[3]/iterations;
}

void DirectionalBlurOperation::deinitExecution() {
	this->inputProgram = NULL;
}

bool DirectionalBlurOperation::determineDependingAreaOfInterest(rcti *input, ReadBufferOperation *readOperation, rcti *output) {
	rcti newInput;

	newInput.xmax = input->xmax + (this->data->distance*this->getWidth());
	newInput.xmin = input->xmin - (this->data->distance*this->getWidth());
	newInput.ymax = input->ymax + (this->data->distance*this->getHeight());
	newInput.ymin = input->ymin - (this->data->distance*this->getHeight());

	return NodeOperation::determineDependingAreaOfInterest(&newInput, readOperation, output);
}
