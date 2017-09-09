# Aug-Sept 2017 Notes

## Documentation

### Add HTML version of tutorials and manuals so that they can be searchable
### Add missing evaluation documents

## System 

### 16bit support for training on Volta GPU (limited functionality)
### Update learner interface to simplify parameter setting and adding new learners (**Potential breaking change**) 
### A preliminary C#/.NET API that enables people to train simple networks such as ConvNet on MNIST. 
### R-binding for training and evaluation (will be published in a separate repository) 
### Improve statistics for distributed evaluation 

## Examples
### Faster R-CNN object detection 
### New example for natural language processing (NLP) 
### Semantic segmentation (stretch goal) 

## Operations
### Noise contrastive estimation node

This provides a built-in efficient (but approximate) loss function used to train networks when the 
number of classes is very large. For example you can use it when you want to predict the next word 
out of a vocabulary of tens or hundreds of thousands of words.

To use it define your loss as 
```python
loss = nce_loss(weights, biases, inputs, labels, noise_distribution)
```
and once you are done training you can make predictions like this
```python
logits = C.times(weights, C.reshape(inputs, (1,), 1)) + biases
```
Note that the noise contrastive estimation loss cannot help with 
reducing inference costs; the cost savings are only during training.

### Improved Attention Layer

The AttentionLayer used to have a couple 

### Aggregation on sparse gradient for embedded layer
### Gradient as an operator (stretch goal) 
### Reduced rank for convolution in C++ to enable convolution on 1D data 
### Dilated convolution 

## Performance 
### Asynchronous evaluation API (Python and C#) 
### Intel MKL update to improve inference speed on CPU by around 2x on AlexNet 

## Keras and Tensorboard 
### Example on Keras and SKLearn multi-GPU support on CNTK 
### Image feature support with Tensorboard for CNTK 

## Others 
### Continue work on [Deep Learning Explained](https://www.edx.org/course/deep-learning-explained-microsoft-dat236x) course on edX. 