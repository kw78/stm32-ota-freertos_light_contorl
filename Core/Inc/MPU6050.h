#ifndef MPU6050_H
#define MPU6050_H

#include "main.h"
#include "i2c.h"
void MPU6050_Init(void);
void MPU6050_Read_Accel(float *ax,float *ay,float *az);
void MPU6050_Cacul_Tangle(float *Pitch,float * Roll,float *ax,float *ay,float *az);
void MPU6050_Read_GYRO(float *tx_g, float *ty_g, float *tz_g);
#endif // !MPU6050_H
