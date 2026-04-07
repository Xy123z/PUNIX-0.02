#include <stdio.h>

int main(){
    int m, r=1, data[10], hamming[20], hamming_corrupt[20];
    char hamming_stylish[20];
   printf("enter the number of data bits: ");
   scanf("%d",&m);
    printf("enter the data bits: ");
    for(int i=1; i<=m;i++){
        scanf("%d",&data[i]);
    }
    while((1 << r) < (m + r + 1))
        r++;
    int h = m + r;
    int k = 1, j = 1;
    for(int i = 1; i<=h; i++){
        if((1 << (k-1)) == i){
            hamming[i] = 0;
            hamming_stylish[i] = 'p';
            k++;
        }
        else{
            hamming[i] = data[j++];
            hamming_stylish[i] = '0';
        }
    }
    for(int i=0; i<r; i++){
        int pos = (1 << i);
        int parity = 0;
        for(int j=1;j<=h;j++){
            if(j & pos)
                parity ^= hamming[j];
            hamming[pos] = parity;
        }
    }
    for(int i=1; i<=h; i++){
        printf("%c ", hamming_stylish[i]);
    }
    printf("hamming code: ");
    for(int i=1;i<=h;i++){
        printf("%d ",hamming[i]);
    }
    printf("\n");
    printf("enter the corrupted data: ");
    for(int i = 1; i<=h;i++){
        scanf("%d",&hamming[i]);
    }
    int error = 0;
for(int i=0; i<r; i++){
    int pos = (1 << i);
    int parity = 0;
    for(int j=1; j<=h; j++){
        if(j & pos){
            parity ^= hamming[j];
        }
    }
    error += (1 << i) * parity;
}
printf("error at bit position: %d\n",error);
printf("corrected hamming code at the receiver side: ");
hamming[error] = !hamming[error];
for(int i=1;i<=h;i++){
    printf("%d ",hamming[i]);
}
printf("\n");
  return 0;
}
